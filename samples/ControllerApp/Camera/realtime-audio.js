"use strict";

class RealtimeAudio {
    static formats(talk = false) {
        const opus = codec => codec.mimeType.toLowerCase() === "audio/opus" && codec.clockRate === 48000;
        const receive = RTCRtpReceiver.getCapabilities("audio")?.codecs.filter(opus) ?? [];
        const send = talk ? RTCRtpSender.getCapabilities("audio")?.codecs.filter(opus) ?? [] : receive;
        return {
            codecs: receive.length && send.length ? ["audio/opus"] : [],
            // Opus uses a 48 kHz RTP clock independently of the encoder's PCM sample rate.
            sampleRates: [48000, 24000, 16000, 12000, 8000],
            bitDepths: [16],
            maximumChannels: 2
        };
    }

    constructor(output, report) {
        this.output = output;
        this.report = report;
        this.closed = true;
        this.generation = 0;
        this.pendingCapture = null;
        this.capture = null;
        this.captureWanted = false;
        this.captureEnabled = false;
        this.captureAttached = false;
        this.received = false;
        this.playing = false;
        this.sent = false;
        this.decoded = false;
        this.decodedSamples = new Map();
        this.senderUpdates = { pending: Promise.resolve(), size: 0 };
    }

    start(peer, profile, talk) {
        if (!this.closed || !profile || profile.codec !== "audio/opus" ||
            !RealtimeAudio.formats(talk).codecs.includes(profile.codec) ||
            !RealtimeAudio.formats(talk).sampleRates.includes(profile.sampleRate) ||
            profile.bitDepth !== 16 || !Number.isInteger(profile.channels) ||
            profile.channels < 1 || profile.channels > 2 || !Number.isInteger(profile.bitRate) ||
            profile.bitRate < 6000 || profile.bitRate > 128000) {
            throw new Error("No compatible audio engine format.");
        }
        this.peer = peer;
        this.profile = profile;
        this.talk = talk === true;
        this.closed = false;
        ++this.generation;
        this.decoded = false;
        this.decodedSamples = new Map();
        this.received = false;
        this.playing = false;
        this.sent = false;
        this.senderUpdates = { pending: Promise.resolve(), size: 0 };
        this.transceiver = peer.addTransceiver("audio", { direction: this.talk ? "sendrecv" : "recvonly" });
        this.transceiver.setCodecPreferences(RTCRtpReceiver.getCapabilities("audio").codecs.filter(
            codec => codec.mimeType.toLowerCase() === "audio/opus" && codec.clockRate === 48000));
        this.sender = this.transceiver.sender;
        this.receiveTrack = null;
        this.lastTime = 0;
        this.inspecting = false;
        this.timer = setInterval(() => this.inspect(), 1000);
        this.deadline = setTimeout(() => {
            if (!this.closed && this.peer === peer && !this.decoded) {
                this.report("error", "Camera audio did not decode within thirty seconds.");
            }
        }, 30000);
    }

    attach(track) {
        if (this.closed || track.kind !== "audio" || this.receiveTrack) {
            track.stop();
            throw new Error("Unexpected audio track.");
        }
        this.receiveTrack = track;
        this.output.srcObject = new MediaStream([track]);
        track.onended = () => {
            if (!this.closed && this.receiveTrack === track) {
                this.stopCapture();
                this.report("error", "The received audio track ended.");
            }
        };
        return this.play();
    }

    async play() {
        if (this.closed || !this.receiveTrack) {
            return;
        }
        const track = this.receiveTrack;
        try {
            await this.output.play();
        } catch (error) {
            if (!this.closed && this.receiveTrack === track) {
                if (error.name === "NotAllowedError") {
                    this.report("playback-blocked");
                } else {
                    this.report("error", "Local audio playback failed.");
                }
            }
        }
    }

    mute(value) {
        this.output.muted = value === true;
        this.playing = false;
        if (!this.closed) {
            this.report(this.output.muted || this.output.volume === 0 ? "muted" : "unmuted");
        }
    }

    volume(value) {
        if (!Number.isFinite(value) || value < 0 || value > 1) {
            throw new Error("Local output volume is outside zero to one.");
        }
        this.output.volume = value;
        this.playing = false;
        if (!this.closed) {
            this.report(this.output.muted || value === 0 ? "muted" : "unmuted");
        }
    }

    async inputs() {
        const devices = await navigator.mediaDevices.enumerateDevices();
        return devices.filter(device => device.kind === "audioinput").slice(0, 32);
    }

    async selectOutput(deviceId) {
        if (typeof deviceId !== "string" || deviceId.length > 1024 || !this.output.setSinkId) {
            throw new Error("Local output selection unsupported.");
        }
        await this.output.setSinkId(deviceId);
    }

    async startCapture(userInitiated, deviceId = "") {
        if (!userInitiated || this.closed || !this.talk || this.peer.connectionState !== "connected" ||
            !["sendrecv", "sendonly"].includes(this.transceiver.currentDirection)) {
            this.report("talk-unsupported");
            return;
        }
        if (this.pendingCapture || this.captureWanted) {
            this.report("capture-pending");
            return;
        }
        if (typeof deviceId !== "string" || deviceId.length > 1024) {
            throw new Error("Invalid local input selection.");
        }
        this.captureWanted = true;
        const generation = ++this.generation;
        const active = () => !this.closed && this.captureWanted && this.generation === generation;
        let timeout;
        let permissionGranted = false;
        const deadline = new Promise((_, reject) => {
            timeout = setTimeout(() => reject(new Error(permissionGranted ? "capture-timeout" : "permission-timeout")), 30000);
        });
        const acquisition = Promise.resolve().then(async () => {
            if (!active()) {
                throw new Error("Capture canceled.");
            }
            const statistics = await this.peer.getStats();
            this.outboundBaseline = new Map();
            for (const [id, report] of statistics) {
                if (report.type === "outbound-rtp" && report.kind === "audio") {
                    this.outboundBaseline.set(id, report.packetsSent ?? 0);
                }
            }
            if (!active()) {
                throw new Error("Capture canceled.");
            }
            return navigator.mediaDevices.getUserMedia({
                video: false,
                audio: {
                    ...(deviceId ? { deviceId: { exact: deviceId } } : {}),
                    channelCount: { exact: this.profile.channels },
                    sampleRate: { ideal: this.profile.sampleRate },
                    echoCancellation: { ideal: true },
                    noiseSuppression: { ideal: true },
                    autoGainControl: { ideal: true }
                }
            });
        }).then(stream => {
            if (!active()) {
                for (const track of stream.getTracks()) {
                    track.stop();
                }
            }
            return stream;
        });
        this.pendingCapture = acquisition;
        const finish = () => {
            if (this.pendingCapture === acquisition) {
                this.pendingCapture = null;
            }
        };
        acquisition.then(finish, finish);
        try {
            const stream = await Promise.race([acquisition, deadline]);
            if (!active()) {
                return;
            }
            permissionGranted = true;
            const tracks = stream.getTracks();
            if (tracks.length !== 1 || tracks[0].kind !== "audio") {
                for (const track of tracks) {
                    track.stop();
                }
                throw new Error("Unexpected capture tracks.");
            }
            this.capture = tracks[0];
            const track = this.capture;
            const sender = this.sender;
            const stop = () => {
                if (this.capture === track) {
                    this.stopCapture();
                }
            };
            this.capture.onended = stop;
            this.capture.onmute = stop;
            this.captureEnabled = true;
            this.captureAttached = false;
            this.sent = false;
            this.report("capture-on");
            const parameters = this.sender.getParameters();
            if (!parameters.encodings?.length) {
                throw new Error("Audio sender bitrate controls unavailable.");
            }
            parameters.encodings[0].maxBitrate = this.profile.bitRate;
            await Promise.race([this.sender.setParameters(parameters), deadline]);
            if (!active()) {
                track.stop();
                return;
            }
            await Promise.race([this.replaceTrack(sender, track), deadline]);
            if (!active()) {
                track.stop();
                return;
            }
            this.captureAttached = true;
        } catch (error) {
            if (active()) {
                this.stopCapture();
                this.report(error.name === "NotAllowedError" ? "permission-denied"
                    : ["permission-timeout", "capture-timeout"].includes(error.message) ? error.message : "capture-unavailable");
            }
        } finally {
            clearTimeout(timeout);
        }
    }

    stopCapture() {
        this.captureWanted = false;
        this.captureEnabled = false;
        this.captureAttached = false;
        ++this.generation;
        if (this.capture) {
            const track = this.capture;
            this.capture = null;
            track.onended = null;
            track.onmute = null;
            track.enabled = false;
            track.stop();
        }
        if (this.sender) {
            const sender = this.sender;
            const generation = this.generation;
            this.replaceTrack(sender, null).catch(() => {
                if (!this.closed && this.sender === sender && this.generation === generation) {
                    this.report("error", "Local audio sender cleanup failed.");
                }
            });
        }
        if (!this.closed) {
            this.report("capture-off");
        }
    }

    get senderQueueSize() {
        return this.senderUpdates.size;
    }

    replaceTrack(sender, track) {
        const updates = this.senderUpdates;
        if (updates.size >= 8) {
            return Promise.reject(new Error("Local audio sender queue overflow."));
        }
        ++updates.size;
        const change = updates.pending.then(() => sender.replaceTrack(track));
        // A failed update must not prevent the subsequent null-track cleanup.
        const completed = () => { --updates.size; };
        updates.pending = change.then(completed, completed);
        return change;
    }

    async inspect() {
        if (this.closed || this.inspecting) {
            return;
        }
        this.inspecting = true;
        const peer = this.peer;
        try {
            const statistics = await this.peer.getStats();
            if (this.closed || this.peer !== peer) {
                return;
            }
            if (!this.captureEnabled && this.sender?.track?.readyState === "live") {
                this.sender.track.enabled = false;
                this.sender.track.stop();
                throw new Error("Unexpected local microphone capture.");
            }
            let decodedProgress = false;
            const samples = new Map();
            for (const [id, report] of statistics) {
                if (report.type === "inbound-rtp" && report.kind === "audio" && report.packetsReceived > 0 && report.codecId) {
                    if (statistics.get(report.codecId)?.mimeType.toLowerCase() !== "audio/opus") {
                        throw new Error("The received audio codec differs from the selected format.");
                    }
                    if (!this.received) {
                        this.received = true;
                        this.report("receiving");
                    }
                    const decoded = report.totalSamplesReceived - (report.concealedSamples ?? 0);
                    if (Number.isSafeInteger(decoded) && decoded > 0) {
                        decodedProgress ||= !this.decodedSamples.has(id) || decoded > this.decodedSamples.get(id);
                        samples.set(id, decoded);
                    }
                }
                if (report.type === "outbound-rtp" && report.kind === "audio" &&
                    this.captureEnabled && this.captureAttached && this.sender.track === this.capture &&
                    report.packetsSent > (this.outboundBaseline?.get(id) ?? 0) && !this.sent) {
                    this.sent = true;
                    this.report("outgoing");
                }
            }
            this.decodedSamples = samples;
            if (decodedProgress) {
                if (!this.decoded) {
                    this.decoded = true;
                    this.report("decoded");
                }
                clearTimeout(this.deadline);
                this.deadline = setTimeout(() => {
                    if (!this.closed && this.peer === peer) {
                        this.report("error", "Decoded audio stopped progressing for ten seconds.");
                    }
                }, 10000);
            }
            const time = this.output.currentTime;
            if (this.playing && (this.output.paused || this.output.muted || this.output.volume === 0)) {
                this.playing = false;
                this.report(this.output.paused ? "playback-blocked" : "muted");
            }
            if (decodedProgress && !this.output.paused && !this.output.muted && this.output.volume > 0 &&
                time > this.lastTime && !this.playing) {
                this.playing = true;
                this.report("playing");
            }
            this.lastTime = time;
        } catch (error) {
            if (!this.closed) {
                this.stopCapture();
                this.report("error", error.message === "Unexpected local microphone capture."
                    ? error.message : "Audio format or media-state validation failed.");
            }
        } finally {
            this.inspecting = false;
        }
    }

    close() {
        this.stopCapture();
        this.closed = true;
        clearInterval(this.timer);
        clearTimeout(this.deadline);
        if (this.receiveTrack) {
            this.receiveTrack.onended = null;
            this.receiveTrack.stop();
            this.receiveTrack = null;
        }
        this.output.pause();
        this.output.srcObject = null;
        this.sender = null;
        this.peer = null;
    }
}

if (typeof module !== "undefined") {
    module.exports = RealtimeAudio;
}
