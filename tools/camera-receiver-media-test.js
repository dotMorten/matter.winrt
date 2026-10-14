"use strict";

void (async () => {
    const results = [];
    const messages = [];
    const contexts = [];
    const sourceTracks = [];
    const elements = [];
    const originalAcquire = navigator.mediaDevices.getUserMedia;
    let camera = null;
    let caseToken = "";
    let failure = null;
    let captureRequests = 0;
    let signaling = Promise.resolve();
    let paintTimer = null;
    let stage = "initialization";
    let deliverySequence = 0;
    const hostDeliveries = new Map();

    function hostSend(command) {
        const testDelivery = ++deliverySequence;
        return new Promise(resolve => {
            hostDeliveries.set(testDelivery, resolve);
            window.chrome.webview.postMessage({ type: "loopback-command", command: { ...command, testDelivery } });
        });
    }

    window.chrome.webview.addEventListener("message", async event => {
        if (event.data?.testDelivery) {
            await delivery;
            publish("loopback-ack", { testDelivery: event.data.testDelivery });
        }
    });

    function require(value, reason) {
        if (!value) {
            throw new Error(reason);
        }
    }

    async function wait(check, reason, milliseconds = 20000) {
        const deadline = performance.now() + milliseconds;
        while (!await check()) {
            if (failure) {
                throw new Error(failure);
            }
            if (performance.now() >= deadline) {
                throw new Error(reason);
            }
            await new Promise(resolve => setTimeout(resolve, 100));
        }
    }

    async function audioSource(frequency) {
        const context = new AudioContext({ sampleRate: 48000 });
        contexts.push(context);
        const oscillator = context.createOscillator();
        const destination = context.createMediaStreamDestination();
        destination.channelCount = 1;
        destination.channelCountMode = "explicit";
        oscillator.frequency.value = frequency;
        oscillator.connect(destination);
        oscillator.start();
        await context.resume();
        const stream = destination.stream;
        sourceTracks.push(...stream.getTracks());
        return stream;
    }

    async function audioSamples(connection) {
        const statistics = await connection.getStats();
        for (const report of statistics.values()) {
            if (report.type === "inbound-rtp" && report.kind === "audio" &&
                statistics.get(report.codecId)?.mimeType.toLowerCase() === "audio/opus") {
                const samples = report.totalSamplesReceived - (report.concealedSamples ?? 0);
                if (samples > 0) {
                    return samples;
                }
            }
        }
        return 0;
    }

    window.cameraMediaTest = {
        accept(message) {
            if (message.type === "loopback-ack") {
                hostDeliveries.get(message.testDelivery)?.();
                hostDeliveries.delete(message.testDelivery);
                return;
            }
            if (message.token !== caseToken) {
                return;
            }
            messages.push(message);
            if (message.type === "error") {
                failure = message.message;
            }
            const target = camera;
            signaling = signaling.then(async () => {
                if (target !== camera || message.token !== caseToken) {
                    return;
                }
                if (message.type === "offer") {
                    await target.setRemoteDescription({ type: "offer", sdp: message.sdp });
                    const answer = await target.createAnswer();
                    await target.setLocalDescription(answer);
                    await hostSend({ type: "answer", token: message.token, sdp: answer.sdp });
                } else if (message.type === "ice") {
                    await target.addIceCandidate({
                        candidate: message.candidate,
                        sdpMid: message.mid,
                        sdpMLineIndex: message.lineIndex
                    });
                }
            }).catch(() => { failure = "Real loopback signaling failed."; });
        }
    };

    async function runCase(listen, talk) {
        const mode = talk ? "synthetic-talk" : listen ? "listening" : "video-only";
        stage = `${mode}:setup`;
        caseToken = crypto.randomUUID().replaceAll("-", "");
        messages.length = 0;
        failure = null;
        signaling = Promise.resolve();
        const beforeCapture = captureRequests;
        camera = new RTCPeerConnection({ iceServers: [] });
        const target = camera;
        const targetToken = caseToken;
        const canvas = document.createElement("canvas");
        canvas.width = 640;
        canvas.height = 480;
        elements.push(canvas);
        document.body.append(canvas);
        const image = canvas.getContext("2d");
        let frame = 0;
        const paint = () => {
            image.fillStyle = `rgb(${++frame % 255}, 80, 120)`;
            image.fillRect(0, 0, 640, 480);
        };
        paint();
        paintTimer = setInterval(paint, 33);
        const videoSource = canvas.captureStream(30);
        sourceTracks.push(...videoSource.getTracks());
        camera.addTrack(videoSource.getVideoTracks()[0], videoSource);
        if (listen) {
            const microphone = await audioSource(440);
            camera.addTrack(microphone.getAudioTracks()[0], microphone);
        }
        camera.ontrack = event => {
            if (camera !== target || caseToken !== targetToken) {
                event.track.stop();
                return;
            }
            require(event.track.kind === "audio" && talk, "Unexpected outgoing track on the real loopback.");
            const sink = document.createElement("audio");
            sink.muted = true;
            sink.srcObject = new MediaStream([event.track]);
            elements.push(sink);
            sourceTracks.push(event.track);
            document.body.append(sink);
            void sink.play().catch(() => { failure = "Muted reverse audio sink failed."; });
        };
        camera.onicecandidate = event => {
            if (event.candidate && camera === target && caseToken === targetToken) {
                void hostSend({
                    type: "ice", token: targetToken,
                    candidates: [{
                        candidate: event.candidate.candidate,
                        sdpMid: event.candidate.sdpMid,
                        sdpMLineIndex: event.candidate.sdpMLineIndex
                    }]
                }).catch(() => { failure = "Real loopback candidate delivery failed."; });
            }
        };
        document.getElementById("audio").muted = true;
        await hostSend({
            type: "start", token: caseToken, videoCodec: 0,
            minimumWidth: 640, maximumWidth: 640, minimumHeight: 480, maximumHeight: 480,
            listen, talk,
            audio: { codec: "audio/opus", channels: 1, sampleRate: 48000, bitRate: 64000, bitDepth: 24 }
        });
        const receiver = peer;
        stage = `${mode}:connection`;
        await wait(() => receiver.connectionState === "connected" && camera.connectionState === "connected",
            "Real ICE/DTLS connection deadline.");
        await wait(() => messages.some(message => message.type === "media" && message.state === "playing"),
            "Real H.264 decoding deadline.");
        require(captureRequests === beforeCapture, "Startup activated synthetic capture unexpectedly.");
        let receivedSamples = 0;
        stage = `${mode}:decoding`;
        if (listen) {
            await wait(async () => {
                receivedSamples = await audioSamples(receiver);
                return receivedSamples > 0 && messages.some(message => message.type === "audio" && message.state === "decoded");
            }, "Real Opus decoding deadline.");
            await wait(() => document.getElementById("audio").currentTime > 0,
                "Muted local playback clock did not progress.");
        }
        let outgoingSamples = 0;
        let talkEvidence = null;
        if (talk) {
            stage = `${mode}:capture`;
            const engine = audioEngine;
            document.getElementById("talk").dispatchEvent(new PointerEvent("pointerdown", { button: 0, bubbles: true }));
            await new Promise(resolve => setTimeout(resolve, 100));
            require(captureRequests === beforeCapture, "Untrusted DOM action activated capture.");
            captureIntent = ++captureSequence;
            publish("capture-request", { press: captureIntent });
            await wait(() => engine.captureEnabled && engine.captureAttached,
                "Real host capture authorization did not reach the WebView.");
            require(engine.captureEnabled && engine.captureAttached, "Synthetic talk track did not attach.");
            const captured = engine.capture;
            stage = `${mode}:reverse-decoding`;
            await wait(async () => {
                outgoingSamples = await audioSamples(camera);
                return outgoingSamples > 0;
            }, "Reverse real Opus decoding deadline.");
            await wait(() => messages.some(message => message.type === "audio" && message.state === "outgoing"),
                "Actual outgoing packet state was not reported.");
            await wait(() => {
                talkEvidence = messages.filter(message => message.type === "talk-diagnostics" &&
                    message.evidence.codec === "audio/opus").at(-1)?.evidence;
                return talkEvidence && talkEvidence.packetsDelta > 0 && talkEvidence.bytesDelta > 0;
            }, "Actual negotiated sender diagnostics were not reported.");
            require(talkEvidence.direction === "sendrecv" && talkEvidence.encodingActive &&
                talkEvidence.trackAttached && talkEvidence.cameraPlayback === "unverified",
                "Real talkback evidence incorrectly describes sender activity or camera playback.");
            if (talkEvidence.inputSignalAvailable) {
                await wait(() => messages.some(message => message.type === "audio" && message.state === "input-signal-observed"),
                    "Available input energy did not observe the synthetic non-silent source.");
            }
            await hostSend({ type: "capture-stop", token: caseToken });
            await wait(() => engine.sender.track === null, "Synthetic talk sender did not detach.");
            require(captured.readyState === "ended" && !engine.captureEnabled,
                "Capture revocation left a live synthetic source.");
        }
        stage = `${mode}:cleanup`;
        await hostSend({ type: "stop", token: caseToken });
        await hostSend({ type: "stop", token: caseToken });
        require(receiver.connectionState === "closed" && peer === null && audioEngine === null,
            "Real peer or audio engine survived Stop.");
        require(document.getElementById("video").srcObject === null && document.getElementById("audio").srcObject === null,
            "Playback elements retained stopped media.");
        camera.close();
        camera = null;
        clearInterval(paintTimer);
        paintTimer = null;
        for (const track of sourceTracks) {
            track.stop();
        }
        for (const context of contexts) {
            await context.close();
        }
        for (const element of elements) {
            element.remove();
        }
        require(sourceTracks.every(track => track.readyState === "ended"), "Synthetic source track cleanup failed.");
        results.push({ mode,
            videoCodec: "video/H264", receivedSamples, outgoingSamples, captureRequests: captureRequests - beforeCapture,
            sourceBitDepth: listen ? 24 : null, pcmPrecision: "browser-managed", talkEvidence,
            tracksEnded: true, peerClosed: true });
        sourceTracks.length = 0;
        contexts.length = 0;
        elements.length = 0;
    }

    try {
        navigator.mediaDevices.getUserMedia = async options => {
            require(options.video === false && options.audio, "Unexpected synthetic capture constraints.");
            ++captureRequests;
            return audioSource(660);
        };
        require(navigator.mediaDevices.getUserMedia !== originalAcquire, "Synthetic capture override unavailable.");
        await runCase(false, false);
        await runCase(true, false);
        await runCase(true, true);
        window.chrome.webview.postMessage({ type: "loopback-result", succeeded: true,
            message: "Real synthetic H.264/Opus loopback and cleanup passed; physical audibility is not validated.",
            details: results });
    } catch (error) {
        window.chrome.webview.postMessage({ type: "loopback-result", succeeded: false,
            message: `${stage}: ${error.message || "Real synthetic loopback failed."}`, details: results });
    } finally {
        navigator.mediaDevices.getUserMedia = originalAcquire;
        clearInterval(paintTimer);
        closeReceiver();
        camera?.close();
        for (const track of sourceTracks) {
            track.stop();
        }
        for (const context of contexts) {
            await context.close();
        }
    }
})();
