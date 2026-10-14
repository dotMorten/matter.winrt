"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const test = require("node:test");

const source = fs.readFileSync(path.join(__dirname, "..", "samples", "ControllerApp", "Camera", "receiver.js"), "utf8");
const audioSource = fs.readFileSync(path.join(__dirname, "..", "samples", "ControllerApp", "Camera", "realtime-audio.js"), "utf8");
const token = "a".repeat(32);
const dimensions = { minimumWidth: 640, maximumWidth: 640, minimumHeight: 480, maximumHeight: 480 };

function receiver(earlyIce = false) {
    const messages = [];
    const peers = [];
    const timers = new Map();
    let timerSequence = 0;
    const video = { srcObject: null, videoWidth: 640, videoHeight: 480,
        requestVideoFrameCallback(callback) { this.frame = callback; } };
    const state = { textContent: "" };
    const elements = new Map();
    const documentHandlers = new Map();
    const windowHandlers = new Map();
    let bridge;
    function element(id) {
        if (!elements.has(id)) {
            elements.set(id, { value: "", textContent: "", hidden: false, disabled: false, volume: 1,
                currentTime: 0, paused: true, srcObject: null, muted: false, handlers: new Map(),
                addEventListener(name, action) { this.handlers.set(name, action); },
                async play() { this.paused = false; }, pause() { this.paused = true; },
                setPointerCapture() {} });
        }
        return elements.get(id);
    }
    const captureRequests = [];
    const captureTracks = [];
    let acquire = async () => {
        const track = { kind: "audio", label: "Synthetic local input", readyState: "live", enabled: true,
            stop() { this.readyState = "ended"; this.stopped = true; } };
        captureTracks.push(track);
        return { getTracks: () => [track] };
    };
    class Peer {
        constructor() { peers.push(this); this.candidates = []; }
        addTransceiver(kind, options) {
            if (kind === "video") {
                assert.equal(options.direction, "recvonly");
            } else {
                assert.equal(kind, "audio");
                this.audio = {
                    currentDirection: options.direction,
                    sender: { track: null, getParameters: () => ({ encodings: [{}] }),
                        async setParameters() {}, async replaceTrack(track) { this.track = track; } },
                    setCodecPreferences(codecs) { assert.ok(codecs.length); }
                };
                return this.audio;
            }
            return { setCodecPreferences(codecs) { assert.ok(codecs.length); } };
        }
        async createOffer() { return { sdp: "v=0\r\nvideo offer" }; }
        async setLocalDescription() {
            if (earlyIce) {
                this.onicecandidate({ candidate: { candidate: "candidate:early", sdpMid: "video", sdpMLineIndex: 0 } });
            }
        }
        async setRemoteDescription(description) { this.description = description; }
        async addIceCandidate(candidate) { this.candidates.push(candidate); }
        async getStats() {
            return new Map([
                ["video", { type: "inbound-rtp", kind: "video", codecId: "codec", framesDecoded: 1 }],
                ["codec", { mimeType: "video/H264" }]
            ]);
        }
        close() { this.closed = true; }
    }
    const context = vm.createContext({
        document: { hidden: false, getElementById: id => id === "video" ? video : id === "state" ? state : element(id),
            addEventListener: (name, action) => documentHandlers.set(name, action) },
        window: { addEventListener: (name, action) => windowHandlers.set(name, action), chrome: { webview: {
            postMessage: message => messages.push(message),
            addEventListener(_, callback) { bridge = callback; }
        } } },
        RTCPeerConnection: Peer,
        RTCRtpReceiver: { getCapabilities: kind => ({ codecs: kind === "audio"
            ? [{ mimeType: "audio/opus", clockRate: 48000, channels: 2 }] : [{ mimeType: "video/H264" }] }) },
        RTCRtpSender: { getCapabilities: () => ({ codecs: [{ mimeType: "audio/opus", clockRate: 48000, channels: 2 }] }) },
        navigator: { mediaDevices: { getUserMedia: options => { captureRequests.push(options); return acquire(); },
            enumerateDevices: async () => [] } },
        MediaStream: class { constructor(tracks) { this.tracks = tracks; } },
        TextEncoder,
        setTimeout(callback) { const id = ++timerSequence; timers.set(id, callback); return id; },
        clearTimeout(id) { timers.delete(id); },
        setInterval(callback) { return callback; }, clearInterval() {}
    });
    vm.runInContext(audioSource, context);
    vm.runInContext(source, context);
    return {
        messages, peers, timers, video, context, element, captureRequests, captureTracks, windowHandlers, documentHandlers,
        acquire: action => { acquire = action; },
        audio: () => vm.runInContext("audioEngine", context),
        bridge: message => bridge({ data: message }),
        send: message => context.receive(message),
        start: (audio = {}) => context.receive({ type: "start", token, videoCodec: 0, ...dimensions, ...audio }),
        stop: () => context.receive({ type: "stop", token })
    };
}

const candidate = { candidate: "candidate:remote", sdpMid: "video", sdpMLineIndex: 0 };

test("offer precedes ICE generated during setLocalDescription", async () => {
    const host = receiver(true);
    await host.start();
    assert.deepEqual(host.messages.map(message => message.type), ["offer", "ice"]);
});

const audioProfile = { codec: "audio/opus", channels: 1, sampleRate: 48000, bitRate: 64000, bitDepth: 16 };
const listening = { listen: true, talk: false, audio: audioProfile };
const talking = { ...listening, talk: true };
async function settle() {
    for (let i = 0; i < 20; ++i) {
        await Promise.resolve();
    }
}
function audioStats(samples = 48000, sent = 0) {
    return new Map([
        ["audio", { type: "inbound-rtp", kind: "audio", codecId: "opus", packetsReceived: 10,
            totalSamplesReceived: samples, concealedSamples: 0 }],
        ["outgoing", { type: "outbound-rtp", kind: "audio", packetsSent: sent }],
        ["opus", { mimeType: "audio/opus" }]
    ]);
}
function connect(host) {
    host.peers.at(-1).connectionState = "connected";
    host.peers.at(-1).onconnectionstatechange();
}
async function hold(host) {
    connect(host);
    host.element("talk").handlers.get("pointerdown")({
        isTrusted: true, button: 0, pointerId: 1, preventDefault() {}
    });
    const request = host.messages.at(-1);
    assert.equal(request.type, "capture-request");
    await host.send({ type: "capture-authorized", token, press: request.press });
    await settle();
}

test("listening is receive-only and cannot acquire a microphone", async () => {
    const host = receiver();
    await host.start(listening);
    assert.equal(host.peers[0].audio.currentDirection, "recvonly");
    const track = { kind: "audio", stop() { this.stopped = true; } };
    host.peers[0].ontrack({ track });
    await settle();
    assert.equal(host.captureRequests.length, 0);
    assert.equal(host.messages.some(message => message.state === "playing"), false);
    host.peers[0].getStats = async () => audioStats();
    host.element("audio").currentTime = 1;
    await host.audio().inspect();
    assert.deepEqual(host.messages.filter(message => message.type === "audio").map(message => message.state),
        ["receiving", "decoded", "playing"]);
    await host.stop();
    assert.equal(track.stopped, true);
    assert.equal(host.element("audio").srcObject, null);
});

test("packets without decoded samples never establish playback", async () => {
    const host = receiver();
    await host.start(listening);
    host.peers[0].getStats = async () => audioStats(0);
    host.element("audio").paused = false;
    host.element("audio").currentTime = 1;
    await host.audio().inspect();
    assert.equal(host.messages.some(message => message.state === "playing"), false);
    [...host.timers.values()][0]();
    assert.equal(host.peers[0].closed, true);
});

test("autoplay denial is playback-blocked, not camera disconnection", async () => {
    const host = receiver();
    await host.start(listening);
    host.element("audio").play = async () => { const error = new Error(); error.name = "NotAllowedError"; throw error; };
    host.peers[0].ontrack({ track: { kind: "audio", stop() {} } });
    await settle();
    assert.equal(host.messages.at(-1).state, "playback-blocked");
    assert.notEqual(host.peers[0].closed, true);
    assert.equal(host.captureRequests.length, 0);
    await host.stop();
});

test("unknown codecs and incompatible formats fail before offering", async () => {
    for (const profile of [{ ...audioProfile, codec: "unknown" }, { ...audioProfile, sampleRate: 44100 },
        { ...audioProfile, bitDepth: 0 }, { ...audioProfile, bitDepth: 20 }]) {
        const host = receiver();
        await assert.rejects(host.start({ ...listening, audio: profile }));
        assert.equal(host.messages.some(message => message.type === "offer"), false);
        await host.stop();
    }
});

test("full-duplex startup does not capture until authorized hold", async () => {
    const host = receiver();
    await host.start(talking);
    assert.equal(host.captureRequests.length, 0);
    await host.audio().startCapture(false);
    assert.equal(host.captureRequests.length, 0);
    await hold(host);
    assert.equal(host.captureRequests.length, 1);
    assert.equal(host.captureRequests[0].video, false);
    assert.equal(host.audio().captureEnabled, true);
    assert.equal(host.element("talk").textContent, "MICROPHONE ACTIVE - release to stop");
    assert.equal(host.element("capture-input").textContent, "Active local input: Synthetic local input (label not exported).");
    assert.equal(JSON.stringify(host.messages).includes("Synthetic local input"), false);
    host.element("talk").handlers.get("pointerup")();
    await settle();
    assert.equal(host.captureTracks[0].stopped, true);
    assert.equal(host.peers[0].audio.sender.track, null);
    await host.stop();
});

test("release before authorization cannot activate capture", async () => {
    const host = receiver();
    await host.start(talking);
    connect(host);
    host.element("talk").handlers.get("pointerdown")({ isTrusted: true, button: 0, pointerId: 1, preventDefault() {} });
    const request = host.messages.at(-1);
    host.element("talk").handlers.get("pointerup")();
    await host.send({ type: "capture-authorized", token, press: request.press });
    await settle();
    assert.equal(host.captureRequests.length, 0);
    await host.stop();
});

test("late permission completion after stop releases tracks and does not block stop", async () => {
    const host = receiver();
    let complete;
    host.acquire(() => new Promise(resolve => { complete = resolve; }));
    await host.start(talking);
    await hold(host);
    await host.stop();
    const track = { kind: "audio", stop() { this.stopped = true; } };
    complete({ getTracks: () => [track] });
    await settle();
    assert.equal(track.stopped, true);
    assert.equal(host.peers[0].audio.sender.track, null);
    assert.equal(host.timers.size, 0);
});

test("permission denial leaves listening intact and microphone off", async () => {
    const host = receiver();
    host.acquire(async () => { const error = new Error(); error.name = "NotAllowedError"; throw error; });
    await host.start(talking);
    await hold(host);
    assert.equal(host.audio().captureEnabled, false);
    assert.equal(host.messages.at(-1).state, "permission-denied");
    assert.notEqual(host.peers[0].closed, true);
    await host.stop();
});

test("permission timeout stops intent and releases a subsequently granted track", async () => {
    const host = receiver();
    let complete;
    host.acquire(() => new Promise(resolve => { complete = resolve; }));
    await host.start(talking);
    await hold(host);
    [...host.timers.values()].at(-1)();
    await settle();
    assert.equal(host.messages.at(-1).state, "permission-timeout");
    const track = { kind: "audio", stop() { this.stopped = true; } };
    complete({ getTracks: () => [track] });
    await settle();
    assert.equal(track.stopped, true);
    await host.stop();
});

test("background, permission revocation, and disconnect stop capture without automatic resume", async () => {
    for (const stop of [
        host => host.windowHandlers.get("blur")(),
        host => host.captureTracks[0].onended(),
        host => { host.peers[0].connectionState = "disconnected"; host.peers[0].onconnectionstatechange(); },
        host => { host.context.document.hidden = true; host.documentHandlers.get("visibilitychange")(); }
    ]) {
        const host = receiver();
        await host.start(talking);
        await hold(host);
        stop(host);
        await settle();
        assert.equal(host.captureTracks[0].stopped, true);
        connect(host);
        await settle();
        assert.equal(host.captureRequests.length, 1);
        assert.equal(host.audio().captureEnabled, false);
        await host.stop();
    }
});

test("old outgoing packet counters do not prove a new transmission", async () => {
    const host = receiver();
    await host.start(talking);
    host.peers[0].getStats = async () => audioStats(48000, 100);
    await hold(host);
    await host.audio().inspect();
    assert.equal(host.messages.some(message => message.state === "outgoing"), false);
    host.peers[0].getStats = async () => audioStats(96000, 101);
    await host.audio().inspect();
    assert.equal(host.messages.some(message => message.state === "outgoing"), true);
    assert.equal(host.messages.at(-1).evidence.cameraPlayback, "unverified");
    await host.stop();
});

test("24-bit source metadata does not claim browser capture PCM precision", async () => {
    const host = receiver();
    await host.start({ ...talking, audio: { ...audioProfile, bitDepth: 24 } });
    assert.equal(host.captureRequests.length, 0);
    await hold(host);
    assert.equal("sampleSize" in host.captureRequests[0].audio, false);
    assert.equal(host.audio().profile.bitDepth, 24);
    await host.stop();
});

test("input energy and current RTP deltas are independent redacted evidence, never camera playback", async () => {
    const host = receiver();
    await host.start(talking);
    await hold(host);
    host.captureTracks[0].id = "owned-input";
    function statistics(energy, packets, bytes) {
        const stats = audioStats(48000, packets);
        Object.assign(stats.get("outgoing"), { codecId: "opus", bytesSent: bytes });
        stats.set("source", { type: "media-source", kind: "audio", trackIdentifier: "owned-input", totalAudioEnergy: energy });
        return stats;
    }
    host.peers[0].getStats = async () => statistics(100, 10, 100);
    await host.audio().inspect();
    let evidence = host.messages.filter(message => message.type === "talk-diagnostics").at(-1).evidence;
    assert.equal(evidence.inputSignalObserved, false, "Old cumulative energy is not new input");
    assert.equal(evidence.inputEnergyDelta, 0);
    host.peers[0].getStats = async () => statistics(100.2, 12, 140);
    await host.audio().inspect();
    evidence = host.messages.filter(message => message.type === "talk-diagnostics").at(-1).evidence;
    assert.equal(evidence.inputSignalObserved, true);
    assert.equal(evidence.packetsDelta, 2);
    assert.equal(evidence.bytesDelta, 40);
    assert.equal(evidence.codec, "audio/opus");
    assert.equal(evidence.cameraPlayback, "unverified");
    assert.equal(evidence.inputSelection, "default");
    assert.equal(host.messages.some(message => message.state === "input-signal-observed"), true);
    const diagnosticKeys = Object.keys(evidence);
    assert.equal(diagnosticKeys.some(key => /sdp|candidate|deviceId|samples|trackIdentifier/i.test(key)), false);
    host.peers[0].getStats = async () => statistics(100.2, 12, 140);
    await host.audio().inspect();
    assert.equal(host.messages.at(-1).evidence.packetsDelta, 0);
    const beforeStop = host.messages.filter(message => message.type === "talk-diagnostics").length;
    await host.stop();
    await host.audio()?.inspect();
    assert.equal(host.messages.filter(message => message.type === "talk-diagnostics").length, beforeStop);
});

test("missing or unrelated input energy never becomes a non-silence claim", async () => {
    const host = receiver();
    await host.start(talking);
    await hold(host);
    host.captureTracks[0].id = "owned-input";
    host.peers[0].getStats = async () => {
        const stats = audioStats(48000, 42);
        stats.set("other", { type: "media-source", kind: "audio", trackIdentifier: "other-input", audioLevel: 1 });
        return stats;
    };
    await host.audio().inspect();
    const evidence = host.messages.filter(message => message.type === "talk-diagnostics").at(-1).evidence;
    assert.equal(evidence.inputSignalAvailable, false);
    assert.equal(evidence.inputSignalObserved, false);
    await host.stop();
});

test("decoded audio stalls and mismatched inbound codecs terminate rather than claiming playback", async () => {
    for (const mismatch of [true, false]) {
        const host = receiver();
        await host.start(listening);
        host.peers[0].getStats = async () => {
            const stats = audioStats();
            if (mismatch) { stats.get("opus").mimeType = "audio/unknown"; }
            return stats;
        };
        await host.audio().inspect();
        if (!mismatch) {
            await host.audio().inspect();
            [...host.timers.values()].at(-1)();
        }
        assert.equal(host.peers[0].closed, true);
        assert.equal(host.messages.some(message => message.state === "playing"), false);
    }
});

test("local mute and volume do not change remote controls or enable capture", async () => {
    const host = receiver();
    await host.start(listening);
    host.audio().mute(true);
    host.audio().volume(0.25);
    assert.equal(host.element("audio").muted, true);
    assert.equal(host.element("audio").volume, 0.25);
    assert.equal(host.captureRequests.length, 0);
    assert.equal(host.messages.some(message => message.type === "offer"), true);
    await host.stop();
});

test("the reusable audio engine works without camera identifiers or bridge", async () => {
    const host = receiver();
    const states = [];
    const Engine = vm.runInContext("RealtimeAudio", host.context);
    const peer = new host.context.RTCPeerConnection();
    const audio = new Engine(host.element("audio"), state => states.push(state));
    audio.start(peer, audioProfile, false);
    audio.mute(true);
    audio.close();
    assert.deepEqual(states, ["muted", "capture-off"]);
    assert.equal(host.messages.length, 0);
    assert.equal(host.captureRequests.length, 0);
});

test("untrusted press and receive-only negotiated directions cannot request a microphone", async () => {
    const host = receiver();
    await host.start(talking);
    connect(host);
    host.element("talk").handlers.get("pointerdown")({ isTrusted: false, button: 0 });
    assert.equal(host.messages.some(message => message.type === "capture-request"), false);
    host.peers[0].audio.currentDirection = "recvonly";
    await host.audio().startCapture(true);
    assert.equal(host.captureRequests.length, 0);
    assert.equal(host.messages.at(-1).state, "talk-unsupported");
    await host.stop();
});

test("privacy stop bypasses a stalled answer operation and old answer cannot change the next session", async () => {
    const host = receiver();
    await host.start(talking);
    await hold(host);
    let complete;
    host.peers[0].setRemoteDescription = () => new Promise(resolve => { complete = resolve; });
    host.bridge({ type: "answer", token, sdp: "v=0\r\nanswer" });
    await settle();
    host.bridge({ type: "capture-stop", token });
    assert.equal(host.captureTracks[0].stopped, true);
    host.bridge({ type: "stop", token });
    assert.equal(host.peers[0].closed, true);
    await host.start();
    complete();
    await settle();
    assert.notEqual(host.peers[1].closed, true);
    assert.equal(host.captureRequests.length, 1);
    await host.stop();
});

test("late sender installation cannot revive a released microphone", async () => {
    const host = receiver();
    await host.start(talking);
    let complete;
    const sender = host.peers[0].audio.sender;
    sender.replaceTrack = track => track ? new Promise(resolve => {
        complete = () => { sender.track = track; resolve(); };
    }) : Promise.resolve().then(() => { sender.track = null; });
    await hold(host);
    host.element("talk").handlers.get("pointerup")();
    assert.equal(host.captureTracks[0].stopped, true);
    complete();
    await settle();
    assert.equal(sender.track, null);
    assert.equal(host.audio().captureEnabled, false);
    await host.stop();
});

test("unexpected local capture is disabled and reported as a privacy failure", async () => {
    const host = receiver();
    await host.start(talking);
    const track = { readyState: "live", enabled: true, stop() { this.stopped = true; } };
    host.peers[0].audio.sender.track = track;
    await host.audio().inspect();
    assert.equal(track.enabled, false);
    assert.equal(track.stopped, true);
    assert.equal(host.peers[0].closed, true);
    assert.equal(host.messages.some(message => message.message === "Unexpected local microphone capture."), true);
});

test("a second inbound audio track is stopped instead of replacing an owned track", async () => {
    const host = receiver();
    await host.start(listening);
    const first = { kind: "audio", stop() { this.stopped = true; } };
    const second = { kind: "audio", stop() { this.stopped = true; } };
    host.peers[0].ontrack({ track: first });
    host.peers[0].ontrack({ track: second });
    await settle();
    assert.equal(first.stopped, true);
    assert.equal(second.stopped, true);
    assert.equal(host.peers[0].closed, true);
});

test("100 listening lifecycles release audio tracks and do not capture", async () => {
    const host = receiver();
    for (let index = 0; index < 100; ++index) {
        await host.start(listening);
        const track = { kind: "audio", stop() { this.stopped = true; } };
        host.peers.at(-1).ontrack({ track });
        await host.stop();
        await host.stop();
        await settle();
        assert.equal(track.stopped, true);
        assert.equal(host.timers.size, 0);
    }
    assert.equal(host.captureRequests.length, 0);
});

test("sender activation timeout stops already granted capture", async () => {
    const host = receiver();
    await host.start(talking);
    host.peers[0].audio.sender.setParameters = () => new Promise(() => {});
    await hold(host);
    assert.equal(host.audio().captureEnabled, true);
    [...host.timers.values()].at(-1)();
    await settle();
    assert.equal(host.captureTracks[0].stopped, true);
    assert.equal(host.messages.at(-1).state, "capture-timeout");
    await host.stop();
});

test("sender changes remain bounded when RTC updates stop responding", async () => {
    const host = receiver();
    await host.start(talking);
    const audio = host.audio();
    host.peers[0].audio.sender.replaceTrack = () => new Promise(() => {});
    const changes = [];
    for (let index = 0; index < 8; ++index) {
        changes.push(audio.replaceTrack(host.peers[0].audio.sender, null));
    }
    await assert.rejects(audio.replaceTrack(host.peers[0].audio.sender, null));
    assert.equal(audio.senderQueueSize, 8);
    await host.stop();
    assert.equal(host.peers[0].closed, true);
});

test("remote ICE is buffered until the answer is applied", async () => {
    const host = receiver();
    await host.start();
    await host.send({ type: "ice", token, candidates: [candidate] });
    assert.equal(host.peers[0].candidates.length, 0);
    await host.send({ type: "answer", token, sdp: "v=0\r\nanswer" });
    assert.equal(host.peers[0].candidates.length, 1);
    assert.equal(host.peers[0].description.type, "answer");
});

test("signaling alone never reports playback", async () => {
    const host = receiver();
    await host.start();
    await host.send({ type: "answer", token, sdp: "v=0\r\nanswer" });
    host.peers[0].ontrack({ track: { kind: "video" } });
    assert.equal(host.messages.some(message => message.type === "media"), false);
    await host.video.frame();
    assert.equal(host.messages.at(-1).state, "playing");
    assert.equal(host.timers.size, 1);
});

test("late decoded frames and ICE cannot leak into a newer receiver", async () => {
    const host = receiver();
    await host.start();
    const old = host.peers[0];
    old.ontrack({ track: { kind: "video" } });
    const frame = host.video.frame;
    await host.stop();
    await host.send({ type: "start", token: "b".repeat(32), videoCodec: 0, ...dimensions });
    const count = host.messages.length;
    await frame();
    old.onicecandidate({ candidate: { candidate: "stale", sdpMid: "video", sdpMLineIndex: 0 } });
    assert.equal(host.messages.length, count);
    assert.equal(old.closed, true);
});

test("unsupported camera codecs are rejected before an offer is generated", async () => {
    for (const videoCodec of [1, 2, 3, 4, -1, null]) {
        const host = receiver();
        await assert.rejects(host.send({ type: "start", token, videoCodec }));
        assert.equal(host.peers.length, 0);
        assert.equal(host.messages.length, 0);
    }
});

test("decoded video with a mismatched negotiated codec fails rather than claiming playback", async () => {
    const host = receiver();
    await host.start();
    host.peers[0].getStats = async () => new Map([
        ["video", { type: "inbound-rtp", kind: "video", codecId: "codec", framesDecoded: 1 }],
        ["codec", { mimeType: "video/VP8" }]
    ]);
    host.peers[0].ontrack({ track: { kind: "video" } });
    await host.video.frame();
    assert.equal(host.messages.at(-1).type, "error");
    assert.equal(host.peers[0].closed, true);
    assert.equal(host.messages.some(message => message.type === "media"), false);
});

test("no decoded frame has a bounded failure and closes the receiver", async () => {
    const host = receiver();
    await host.start();
    [...host.timers.values()][0]();
    assert.equal(host.messages.at(-1).type, "error");
    assert.equal(host.peers[0].closed, true);
});

test("allocation dimensions are required and ordered before creating a peer", async () => {
    for (const invalid of [
        { minimumWidth: 0 }, { maximumWidth: 65536 }, { minimumHeight: null },
        { maximumHeight: 479 }, { minimumWidth: 641 }
    ]) {
        const host = receiver();
        await assert.rejects(host.send({ type: "start", token, videoCodec: 0, ...dimensions, ...invalid }));
        assert.equal(host.peers.length, 0);
    }
});

test("decoded dimensions outside the allocation fail without reporting playback", async () => {
    for (const width of [0, 639, 641]) {
        const host = receiver();
        await host.start();
        host.video.videoWidth = width;
        host.peers[0].ontrack({ track: { kind: "video" } });
        await host.video.frame();
        assert.equal(host.messages.at(-1).type, "error");
        assert.equal(host.peers[0].closed, true);
        assert.equal(host.messages.some(message => message.type === "media"), false);
    }
});

test("stats lag behind a presented frame without falsely claiming playback", async () => {
    const host = receiver();
    await host.start();
    const stats = host.peers[0].getStats;
    host.peers[0].getStats = async () => new Map();
    host.peers[0].ontrack({ track: { kind: "video" } });
    await host.video.frame();
    assert.equal(host.messages.some(message => message.type === "media"), false);
    assert.notEqual(host.peers[0].closed, true);
    host.peers[0].getStats = stats;
    await host.video.frame();
    assert.equal(host.messages.at(-1).state, "playing");
});

test("repeated presented callbacks do not conceal a stalled decoder", async () => {
    const host = receiver();
    await host.start();
    host.peers[0].ontrack({ track: { kind: "video" } });
    await host.video.frame();
    const deadline = [...host.timers.values()][0];
    await host.video.frame();
    assert.equal([...host.timers.values()][0], deadline);
    deadline();
    assert.equal(host.messages.at(-1).type, "error");
    assert.equal(host.peers[0].closed, true);
});

test("new decoded frames renew the watchdog without duplicate playback events", async () => {
    const host = receiver();
    await host.start();
    host.peers[0].ontrack({ track: { kind: "video" } });
    await host.video.frame();
    const deadline = [...host.timers.values()][0];
    host.peers[0].getStats = async () => new Map([
        ["video", { type: "inbound-rtp", kind: "video", codecId: "codec", framesDecoded: 2 }],
        ["codec", { mimeType: "video/H264" }]
    ]);
    await host.video.frame();
    assert.notEqual([...host.timers.values()][0], deadline);
    assert.equal(host.messages.filter(message => message.type === "media").length, 1);
    await host.stop();
    deadline();
    assert.equal(host.timers.size, 0);
    assert.equal(host.messages.at(-1).state, "playing");
});

test("unexpected audio tracks close the video-only receiver", async () => {
    const host = receiver();
    await host.start();
    host.peers[0].ontrack({ track: { kind: "audio" } });
    assert.equal(host.messages.at(-1).type, "error");
    assert.equal(host.peers[0].closed, true);
});

test("a replacement RTP stream's decoded counter renews the watchdog", async () => {
    const host = receiver();
    await host.start();
    host.peers[0].ontrack({ track: { kind: "video" } });
    await host.video.frame();
    const deadline = [...host.timers.values()][0];
    host.peers[0].getStats = async () => new Map([
        ["replacement", { type: "inbound-rtp", kind: "video", codecId: "codec", framesDecoded: 1 }],
        ["codec", { mimeType: "video/H264" }]
    ]);
    await host.video.frame();
    assert.notEqual([...host.timers.values()][0], deadline);
    assert.notEqual(host.peers[0].closed, true);
});

test("oversized SDP, ICE, MID, reserved index, and stale token are rejected", async () => {
    const host = receiver();
    await host.start();
    await assert.rejects(host.send({ type: "answer", token, sdp: "x".repeat(16385) }));
    for (const invalid of [
        { ...candidate, candidate: "x".repeat(2049) },
        { ...candidate, candidate: "" },
        { ...candidate, sdpMid: "x".repeat(257) },
        { ...candidate, sdpMLineIndex: 65535 }
    ]) {
        await assert.rejects(host.send({ type: "ice", token, candidates: [invalid] }));
    }
    await assert.rejects(host.send({ type: "stop", token: "b".repeat(32) }));
    assert.notEqual(host.peers[0].closed, true);
});

test("remote candidate batches and pre-answer queues are bounded", async () => {
    const host = receiver();
    await host.start();
    await assert.rejects(host.send({ type: "ice", token, candidates: Array(17).fill(candidate) }));
    await host.send({ type: "ice", token, candidates: Array(16).fill(candidate) });
    await host.send({ type: "ice", token, candidates: Array(16).fill(candidate) });
    await assert.rejects(host.send({ type: "ice", token, candidates: [candidate] }));
});

test("100 receiver starts and stops release peers, tracks, and timers", async () => {
    const host = receiver();
    for (let i = 0; i < 100; ++i) {
        await host.start();
        await host.stop();
        assert.equal(host.video.srcObject, null);
        assert.equal(host.timers.size, 0);
    }
    assert.equal(host.peers.length, 100);
    assert.ok(host.peers.every(peer => peer.closed));
});
