"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const test = require("node:test");

const source = fs.readFileSync(path.join(__dirname, "..", "samples", "ControllerApp", "Camera", "receiver.js"), "utf8");
const token = "a".repeat(32);

function receiver(earlyIce = false) {
    const messages = [];
    const peers = [];
    const timers = new Map();
    const video = { srcObject: null, requestVideoFrameCallback(callback) { this.frame = callback; } };
    const state = { textContent: "" };
    class Peer {
        constructor() { peers.push(this); this.candidates = []; }
        addTransceiver(kind, options) {
            assert.equal(kind, "video");
            assert.equal(options.direction, "recvonly");
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
                ["video", { type: "inbound-rtp", kind: "video", codecId: "codec" }],
                ["codec", { mimeType: "video/H264" }]
            ]);
        }
        close() { this.closed = true; }
    }
    const context = vm.createContext({
        document: { getElementById: id => id === "video" ? video : state },
        window: { chrome: { webview: {
            postMessage: message => messages.push(message),
            addEventListener() {}
        } } },
        RTCPeerConnection: Peer,
        RTCRtpReceiver: { getCapabilities: () => ({ codecs: [{ mimeType: "video/H264" }] }) },
        MediaStream: class { constructor(tracks) { this.tracks = tracks; } },
        TextEncoder,
        setTimeout(callback) { const id = timers.size + 1; timers.set(id, callback); return id; },
        clearTimeout(id) { timers.delete(id); }
    });
    vm.runInContext(source, context);
    return {
        messages, peers, timers, video,
        send: message => context.receive(message),
        start: () => context.receive({ type: "start", token, videoCodec: 0 }),
        stop: () => context.receive({ type: "stop", token })
    };
}

const candidate = { candidate: "candidate:remote", sdpMid: "video", sdpMLineIndex: 0 };

test("offer precedes ICE generated during setLocalDescription", async () => {
    const host = receiver(true);
    await host.start();
    assert.deepEqual(host.messages.map(message => message.type), ["offer", "ice"]);
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
    host.peers[0].ontrack({ track: {} });
    assert.equal(host.messages.some(message => message.type === "media"), false);
    await host.video.frame();
    assert.equal(host.messages.at(-1).state, "playing");
    assert.equal(host.timers.size, 0);
});

test("late decoded frames and ICE cannot leak into a newer receiver", async () => {
    const host = receiver();
    await host.start();
    const old = host.peers[0];
    old.ontrack({ track: {} });
    const frame = host.video.frame;
    await host.stop();
    await host.send({ type: "start", token: "b".repeat(32), videoCodec: 0 });
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
        ["video", { type: "inbound-rtp", kind: "video", codecId: "codec" }],
        ["codec", { mimeType: "video/VP8" }]
    ]);
    host.peers[0].ontrack({ track: {} });
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
