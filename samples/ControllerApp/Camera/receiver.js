"use strict";

let peer = null;
let token = null;
let remoteDescriptionSet = false;
let pendingRemoteCandidates = [];
let pendingLocalCandidates = [];
let offerPublished = false;
let mediaDeadline = null;
const video = document.getElementById("video");
const state = document.getElementById("state");

function publish(type, fields = {}) {
    window.chrome.webview.postMessage({ token, type, ...fields });
}

function closeReceiver() {
    clearTimeout(mediaDeadline);
    mediaDeadline = null;
    if (peer) {
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
                return;
            }
            if (event.track.kind !== "video") {
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
            if (peer === ownedPeer && (peer.connectionState === "failed" || peer.connectionState === "disconnected")) {
                publish("media", { state: peer.connectionState });
            }
        };
        video.onerror = () => {
            if (peer === ownedPeer) {
                publish("error", { message: "Video decoding failed." });
            }
        };
        const offer = await peer.createOffer();
        await peer.setLocalDescription(offer);
        if (!peer) {
            throw new Error("Receiver closed during offer generation.");
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
    if (!peer) {
        throw new Error("Receiver is not initialized.");
    }
    if (message.type === "answer") {
        if (typeof message.sdp !== "string" || new TextEncoder().encode(message.sdp).length > 16384) {
            throw new Error("Answer exceeds receiver bounds.");
        }
        await peer.setRemoteDescription({ type: "answer", sdp: message.sdp });
        remoteDescriptionSet = true;
        for (const candidate of pendingRemoteCandidates) {
            await peer.addIceCandidate(candidate);
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
                await peer.addIceCandidate(candidate);
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

let delivery = Promise.resolve();
window.chrome.webview.addEventListener("message", event => {
    delivery = delivery.then(() => receive(event.data)).catch(() => {
        state.textContent = "Receiver failed.";
        publish("error", { message: "Receiver rejected signaling or could not establish media." });
        closeReceiver();
    });
});
