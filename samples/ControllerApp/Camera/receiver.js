"use strict";

let peer = null;
let token = null;
let remoteDescriptionSet = false;
let pendingRemoteCandidates = [];
let pendingLocalCandidates = [];
let offerPublished = false;
let mediaDeadline = null;
let audioEngine = null;
let captureIntent = 0;
let captureSequence = 0;
const video = document.getElementById("video");
const state = document.getElementById("state");
const audioState = document.getElementById("audio-state");
const audioControls = document.getElementById("audio-controls");
const talkButton = document.getElementById("talk");
const input = document.getElementById("input");

function publish(type, fields = {}) {
    window.chrome.webview.postMessage({ token, type, ...fields });
}

function closeReceiver() {
    releaseCapture();
    audioEngine?.close();
    audioEngine = null;
    clearTimeout(mediaDeadline);
    mediaDeadline = null;
    if (peer) {
        for (const receiver of peer.getReceivers?.() ?? []) {
            receiver.track?.stop();
        }
        peer.close();
    }
    peer = null;
    video.srcObject = null;
    pendingRemoteCandidates = [];
    pendingLocalCandidates = [];
    offerPublished = false;
    remoteDescriptionSet = false;
}

async function receive(message) {
    if (!message || typeof message !== "object") {
        throw new Error("Invalid receiver command.");
    }
    if (message.type === "start") {
        if (typeof message.token !== "string" || message.token.length !== 32 || peer) {
            throw new Error("Invalid receiver initialization.");
        }
        token = message.token;
        if (message.talk === true && message.listen !== true) {
            throw new Error("Talkback requires explicit listening.");
        }
        const capabilities = RTCRtpReceiver.getCapabilities("video");
        if (!capabilities || capabilities.codecs.length === 0) {
            throw new Error("This receiver has no supported video codecs.");
        }
        const codecMimeTypes = ["video/h264", "video/h265", "video/h266", "video/av1"];
        if (!Number.isInteger(message.videoCodec) || message.videoCodec < 0 || message.videoCodec >= codecMimeTypes.length) {
            throw new Error("The selected camera codec is unsupported.");
        }
        const selectedMimeType = codecMimeTypes[message.videoCodec];
        const selectedCodecs = capabilities.codecs.filter(codec => codec.mimeType.toLowerCase() === selectedMimeType);
        if (selectedCodecs.length === 0) {
            throw new Error("The selected stream codec is not supported by this receiver.");
        }
        for (const value of [message.minimumWidth, message.maximumWidth, message.minimumHeight, message.maximumHeight]) {
            if (!Number.isInteger(value) || value < 1 || value > 65535) {
                throw new Error("Invalid allocated video dimensions.");
            }
        }
        if (message.minimumWidth > message.maximumWidth || message.minimumHeight > message.maximumHeight) {
            throw new Error("Reversed allocated video dimensions.");
        }
        peer = new RTCPeerConnection({ iceServers: [], iceTransportPolicy: "all" });
        const ownedPeer = peer;
        audioControls.hidden = message.listen !== true;
        talkButton.hidden = message.talk !== true;
        talkButton.disabled = true;
        if (message.listen === true) {
            audioEngine = new RealtimeAudio(document.getElementById("audio"), (audioStatus, error, evidence) => {
                if (peer !== ownedPeer) {
                    return;
                }
                if (audioStatus === "error") {
                    publish("error", { message: error });
                    closeReceiver();
                    return;
                }
                if (audioStatus === "diagnostics") {
                    audioState.textContent = `Capture enabled; input signal ${evidence.inputSignalObserved ? "observed" : "not observed"}; ` +
                        `RTP delta ${evidence.packetsDelta}; camera playback UNVERIFIED.`;
                    publish("talk-diagnostics", { evidence });
                    return;
                }
                audioState.textContent = `Audio: ${audioStatus}. Camera talkback playback unverified.`;
                if (audioStatus === "capture-on") {
                    talkButton.textContent = "MICROPHONE ACTIVE - release to stop";
                    const label = audioEngine.capture?.label;
                    document.getElementById("capture-input").textContent = typeof label === "string" && label
                        ? `Active local input: ${label.slice(0, 128)} (label not exported).`
                        : "Active local input: device label unavailable.";
                } else if (audioStatus === "outgoing") {
                    talkButton.textContent = "OUTGOING RTP - camera playback unverified - release to stop";
                } else if (["capture-off", "permission-denied", "permission-timeout", "capture-unavailable",
                    "talk-unsupported", "capture-timeout"].includes(audioStatus)) {
                    talkButton.textContent = "Hold to talk (microphone off)";
                    document.getElementById("capture-input").textContent = "Local input inactive; device labels stay local.";
                }
                publish("audio", { state: audioStatus });
            });
            audioEngine.start(peer, message.audio, message.talk === true);
        }
        let framesDecoded = new Map();
        let playing = false;
        mediaDeadline = setTimeout(() => {
            if (peer === ownedPeer) {
                publish("error", { message: "No decoded video frame within the receiver deadline." });
                closeReceiver();
            }
        }, 30000);
        const transceiver = peer.addTransceiver("video", { direction: "recvonly" });
        transceiver.setCodecPreferences(selectedCodecs);
        peer.onicecandidate = event => {
            if (peer !== ownedPeer) {
                return;
            }
            if (event.candidate) {
                const candidate = { candidate: event.candidate.candidate, mid: event.candidate.sdpMid,
                                    lineIndex: event.candidate.sdpMLineIndex };
                if (offerPublished) {
                    publish("ice", candidate);
                } else if (pendingLocalCandidates.length < 32) {
                    pendingLocalCandidates.push(candidate);
                } else {
                    publish("error", { message: "Local candidate queue overflow." });
                    closeReceiver();
                }
            }
        };
        peer.ontrack = event => {
            if (peer !== ownedPeer) {
                event.track.stop?.();
                return;
            }
            if (event.track.kind === "audio" && audioEngine) {
                try {
                    void audioEngine.attach(event.track);
                } catch {
                    publish("error", { message: "Unexpected audio track or playback initialization failure." });
                    closeReceiver();
                }
                return;
            }
            if (event.track.kind !== "video") {
                event.track.stop?.();
                publish("error", { message: "Unexpected non-video media track." });
                closeReceiver();
                return;
            }
            video.srcObject = new MediaStream([event.track]);
            state.textContent = "Media track received; waiting for decoded frames.";
            const inspectFrame = async () => {
                if (peer !== ownedPeer) {
                    return;
                }
                try {
                    const statistics = await ownedPeer.getStats();
                    if (peer !== ownedPeer) {
                        return;
                    }
                    let negotiated = true;
                    let decoded = 0;
                    let progressed = false;
                    const currentFrames = new Map();
                    statistics.forEach((report, id) => {
                        if (report.type === "inbound-rtp" && report.kind === "video" && report.codecId &&
                            Number.isSafeInteger(report.framesDecoded) && report.framesDecoded > 0) {
                            const codec = statistics.get(report.codecId);
                            negotiated = negotiated && codec && codec.mimeType.toLowerCase() === selectedMimeType;
                            decoded += report.framesDecoded;
                            currentFrames.set(id, report.framesDecoded);
                            progressed ||= !framesDecoded.has(id) || report.framesDecoded > framesDecoded.get(id);
                        }
                    });
                    if (decoded === 0) {
                        video.requestVideoFrameCallback(inspectFrame);
                        return;
                    }
                    if (!negotiated) {
                        throw new Error("The received video codec does not match the selected camera stream.");
                    }
                    if (video.videoWidth < message.minimumWidth || video.videoWidth > message.maximumWidth ||
                        video.videoHeight < message.minimumHeight || video.videoHeight > message.maximumHeight) {
                        throw new Error("Decoded video dimensions do not match the allocated stream.");
                    }
                    framesDecoded = currentFrames;
                    if (progressed) {
                        clearTimeout(mediaDeadline);
                        mediaDeadline = setTimeout(() => {
                            if (peer === ownedPeer) {
                                publish("error", { message: "Decoded video stopped progressing for ten seconds." });
                                closeReceiver();
                            }
                        }, 10000);
                    }
                } catch (error) {
                    if (peer === ownedPeer) {
                        publish("error", { message: error.message || "Decoded video or allocated-stream validation failed." });
                        closeReceiver();
                    }
                    return;
                }
                if (!playing) {
                    playing = true;
                    state.textContent = "Live video frame decoded.";
                    publish("media", { state: "playing", codec: selectedMimeType });
                }
                if (peer === ownedPeer) {
                    video.requestVideoFrameCallback(inspectFrame);
                }
            };
            video.requestVideoFrameCallback(inspectFrame);
        };
        peer.onconnectionstatechange = () => {
            if (peer === ownedPeer) {
                talkButton.disabled = ownedPeer.connectionState !== "connected";
            }
            if (peer === ownedPeer && (peer.connectionState === "failed" || peer.connectionState === "disconnected")) {
                releaseCapture();
                publish("media", { state: peer.connectionState });
            }
        };
        video.onerror = () => {
            if (peer === ownedPeer) {
                publish("error", { message: "Video decoding failed." });
            }
        };
        const offer = await ownedPeer.createOffer();
        if (peer !== ownedPeer) {
            return;
        }
        await ownedPeer.setLocalDescription(offer);
        if (peer !== ownedPeer) {
            return;
        }
        publish("offer", { sdp: offer.sdp });
        offerPublished = true;
        for (const candidate of pendingLocalCandidates) {
            publish("ice", candidate);
        }
        pendingLocalCandidates = [];
        return;
    }
    if (message.token !== token) {
        throw new Error("Receiver session mismatch.");
    }
    if (message.type === "stop") {
        closeReceiver();
        state.textContent = "Closed.";
        return;
    }
    if (message.type === "capture-stop") {
        releaseCapture();
        return;
    }
    if (!peer) {
        throw new Error("Receiver is not initialized.");
    }
    const targetPeer = peer;
    if (message.type === "capture-authorized") {
        if (audioEngine && captureIntent !== 0 && captureIntent === message.press && !document.hidden) {
            const ownedAudioEngine = audioEngine;
            void ownedAudioEngine.startCapture(true, input.value).catch(() => {
                if (audioEngine === ownedAudioEngine) {
                    publish("error", { message: "Invalid local microphone request." });
                    closeReceiver();
                }
            });
        } else {
            releaseCapture();
        }
    } else if (message.type === "answer") {
        if (typeof message.sdp !== "string" || new TextEncoder().encode(message.sdp).length > 16384) {
            throw new Error("Answer exceeds receiver bounds.");
        }
        await targetPeer.setRemoteDescription({ type: "answer", sdp: message.sdp });
        if (peer !== targetPeer) {
            return;
        }
        remoteDescriptionSet = true;
        for (const candidate of pendingRemoteCandidates) {
            await targetPeer.addIceCandidate(candidate);
            if (peer !== targetPeer) {
                return;
            }
        }
        pendingRemoteCandidates = [];
        state.textContent = "Signaling answer applied; waiting for media.";
    } else if (message.type === "ice") {
        if (!Array.isArray(message.candidates) || message.candidates.length > 16) {
            throw new Error("Invalid remote candidate batch.");
        }
        for (const candidate of message.candidates) {
            if (typeof candidate.candidate !== "string" ||
                candidate.candidate.length === 0 ||
                new TextEncoder().encode(candidate.candidate).length > 2048 ||
                (candidate.sdpMid !== null && (typeof candidate.sdpMid !== "string" ||
                    new TextEncoder().encode(candidate.sdpMid).length > 256)) ||
                (candidate.sdpMLineIndex !== null && (!Number.isInteger(candidate.sdpMLineIndex) ||
                    candidate.sdpMLineIndex < 0 || candidate.sdpMLineIndex >= 65535))) {
                throw new Error("Invalid remote candidate.");
            }
            if (remoteDescriptionSet) {
                await targetPeer.addIceCandidate(candidate);
                if (peer !== targetPeer) {
                    return;
                }
            } else {
                if (pendingRemoteCandidates.length === 32) {
                    throw new Error("Remote candidate queue overflow.");
                }
                pendingRemoteCandidates.push(candidate);
            }
        }
    } else {
        throw new Error("Unsupported receiver command.");
    }
}

function releaseCapture() {
    captureIntent = 0;
    audioEngine?.stopCapture();
    if (audioEngine && token) {
        publish("capture-release");
    }
}

talkButton.addEventListener("pointerdown", event => {
    if (!event.isTrusted || event.button !== 0 || talkButton.disabled || !audioEngine?.talk || document.hidden) {
        return;
    }
    event.preventDefault();
    talkButton.setPointerCapture(event.pointerId);
    captureIntent = ++captureSequence;
    publish("capture-request", { press: captureIntent });
});
talkButton.addEventListener("keydown", event => {
    if (event.isTrusted && !event.repeat && ["Space", "Enter"].includes(event.code) &&
        !talkButton.disabled && audioEngine?.talk && !document.hidden) {
        event.preventDefault();
        captureIntent = ++captureSequence;
        publish("capture-request", { press: captureIntent });
    }
});
talkButton.addEventListener("keyup", event => {
    if (["Space", "Enter"].includes(event.code)) {
        releaseCapture();
    }
});
for (const name of ["pointerup", "pointercancel", "lostpointercapture", "blur"]) {
    talkButton.addEventListener(name, releaseCapture);
}
document.addEventListener("visibilitychange", () => {
    if (document.hidden) {
        releaseCapture();
    }
});
window.addEventListener("blur", releaseCapture);
document.getElementById("resume-audio").addEventListener("click", () => void audioEngine?.play());
document.getElementById("local-mute").addEventListener("change", event => audioEngine?.mute(event.target.checked));
document.getElementById("local-volume").addEventListener("input", event => audioEngine?.volume(Number(event.target.value)));
input.addEventListener("change", releaseCapture);
document.getElementById("devices").addEventListener("click", async () => {
    if (!audioEngine) {
        return;
    }
    try {
        const devices = await audioEngine.inputs();
        input.replaceChildren(new Option("Default microphone", ""));
        devices.filter(device => device.deviceId).forEach((device, index) => {
            input.add(new Option(device.label || `Microphone ${index + 1}`, device.deviceId));
        });
        const output = document.getElementById("output");
        output.replaceChildren(new Option("Default output", ""));
        (await navigator.mediaDevices.enumerateDevices()).filter(device => device.kind === "audiooutput" && device.deviceId)
            .slice(0, 32).forEach((device, index) => output.add(new Option(device.label || `Output ${index + 1}`, device.deviceId)));
    } catch {
        audioState.textContent = "Local audio device enumeration unavailable.";
    }
});
document.getElementById("output").addEventListener("change", async event => {
    try {
        await audioEngine?.selectOutput(event.target.value);
    } catch {
        audioState.textContent = "Local output selection unsupported or denied; the camera remains connected.";
    }
});

let delivery = Promise.resolve();
window.chrome.webview.addEventListener("message", event => {
    // Stop/capture revocation must not wait behind an outstanding RTC operation.
    if (event.data?.token === token && ["stop", "capture-stop"].includes(event.data.type)) {
        void receive(event.data);
        return;
    }
    delivery = delivery.then(() => receive(event.data)).catch(() => {
        state.textContent = "Receiver failed.";
        publish("error", { message: "Receiver rejected signaling or could not establish media." });
        closeReceiver();
    });
});
