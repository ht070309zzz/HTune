"""Summarize captured observations; never turn absent ground truth into a pass."""
import argparse
import collections
import datetime
import hashlib
import json
import math
from pathlib import Path

def summarize(path):
    original = path.read_bytes()
    rows, invalid_lines = [], []
    for number, line in enumerate(original.splitlines(), 1):
        if not line.strip():
            continue
        try:
            row = json.loads(line.decode("utf-8-sig"))
            if not isinstance(row, dict):
                raise ValueError("Record is not an object")
            rows.append(row)
        except (UnicodeError, ValueError):
            invalid_lines.append(number)
    samples = [row for row in rows if "pid" in row and "event" not in row]
    events = [row for row in rows if "event" in row]
    def local(utc):
        try:
            if not isinstance(utc, str):
                return None
            value = datetime.datetime.fromisoformat(utc.replace("Z", "+00:00"))
            if value.tzinfo is None:
                return None
            return value.astimezone(datetime.timezone(datetime.timedelta(hours=8))).isoformat()
        except ValueError:
            return None
    def numeric(value):
        if isinstance(value, bool):
            return False
        if isinstance(value, int):
            return -(1 << 63) <= value < (1 << 63)
        return isinstance(value, float) and math.isfinite(value)
    def text(row, key, default=""):
        value = row.get(key)
        return value if isinstance(value, str) else default
    def writer_session(row):
        value = row.get("logSession")
        return value if isinstance(value, str) and 0 < len(value) <= 128 else None
    def audio(row):
        value = row.get("audio")
        return value if isinstance(value, dict) else {}
    gate_reasons = {
        "eligible": {"independent-preview", "post-completion-identity", "unverified-metadata"},
        "wait": {"identity-reacquiring", "observation-missing", "observation-from-future",
                 "awaiting-post-completion-observation", "observation-expired"},
        "drop": {"generation-changed", "application-exiting", "request-cancelled", "process-instance-changed",
                 "metadata-unavailable", "identity-unverified", "song-changed", "identity-reacquiring",
                 "new-request", "verified-identity-arrived", "manual-selection", "follow-current",
                 "replaced-result", "playback-boundary-changed"},
    }
    def unsigned_integer(value, maximum):
        return isinstance(value, int) and not isinstance(value, bool) and 0 <= value <= maximum
    def unsigned_decimal(value):
        return (isinstance(value, str) and 0 < len(value) <= 20 and value.isascii() and value.isdecimal()
                and (value == "0" or value[0] != "0") and int(value) < (1 << 64))
    def song_id(value):
        return (isinstance(value, str) and (value == "" or
                (len(value) <= 19 and value.isascii() and value.isdecimal() and any(c != "0" for c in value))))
    def valid_gate(row):
        decision, reason, stage = row.get("decision"), row.get("reason"), row.get("stage")
        return (type(row.get("lyricGateSchema")) is int and row["lyricGateSchema"] == 1
                and writer_session(row) is not None
                and isinstance(decision, str) and decision in gate_reasons
                and isinstance(reason, str) and reason in gate_reasons[decision]
                and stage in ("result-arrival", "pending-poll", "pending-retired")
                and (stage != "pending-retired" or decision == "drop")
                and all(unsigned_integer(row.get(key), (1 << 32) - 1) for key in ("requestPid", "currentPid"))
                and all(unsigned_decimal(row.get(key)) for key in
                        ("requestGeneration", "currentGeneration", "requestProcessCreatedAtTicks", "currentProcessCreatedAtTicks"))
                and all(song_id(row.get(key)) for key in ("requestPlaybackId", "lyricId", "currentSongId", "resultSongId"))
                and all(unsigned_integer(row.get(key), (1 << 63) - 1) for key in
                        ("monotonicMs", "completedAtMs", "observedAtMs", "serializedAtMonotonicMs"))
                and row.get("utcIsSerializationTime") is True
                and row["serializedAtMonotonicMs"] >= row["monotonicMs"]
                and unsigned_integer(row.get("lineCount"), 20000)
                and unsigned_integer(row.get("resultState"), 6)
                and all(type(row.get(key)) is bool for key in
                        ("songIdVerified", "metadataStale", "identityReacquiring", "fromCache", "wordTimed"))
                and row.get("origin") in ("native-id", "confirmed-candidate", "automatic-candidate", "local-import", "manual-input")
                and row.get("waitUpdatesCoalesced") is True
                and "displayConfirmed" in row and row["displayConfirmed"] is None)
    gate_events = [row for row in events if row.get("event") == "lyric-commit-gate"]
    recognized_gates = [row for row in gate_events if valid_gate(row)]
    malformed_gates = len(gate_events) - len(recognized_gates)
    gate_trace_losses = {}
    malformed_gate_losses = 0
    for row in events:
        if row.get("event") != "lyric-commit-trace-loss":
            continue
        session, value = row.get("logSession"), row.get("droppedTraceEvents")
        if (not isinstance(session, str) or not 0 < len(session) <= 128
                or type(row.get("lyricGateSchema")) is not int or row["lyricGateSchema"] != 1
                or not unsigned_integer(value, (1 << 64) - 1)):
            malformed_gate_losses += 1
            continue
        gate_trace_losses[session] = max(gate_trace_losses.get(session, 0), value)
    reported_gate_losses = sum(gate_trace_losses.values())
    # Loss counters are cumulative PER SESSION, so sum each session's maxima,
    # never every marker and never just one maximum across appended sessions.
    counter_names = ("droppedSamples", "droppedEvents", "serializationFailures", "oversizedRows")
    losses = {}
    started_sessions, closed_sessions = set(), set()
    malformed_controls = 0
    for row in events:
        kind = row.get("event")
        session = row.get("logSession")
        valid_session = isinstance(session, str) and 0 < len(session) <= 128
        if kind == "log-start" and valid_session:
            started_sessions.add(session)
        if kind == "log-end" and valid_session and row.get("queueDrained") is True and row.get("reason") in ("stop", "limit"):
            closed_sessions.add(session)
        if kind not in ("log-loss", "log-end"):
            continue
        counters = losses.setdefault(session if valid_session else "legacy-unknown-session", dict.fromkeys(counter_names, 0))
        for name in counter_names:
            value = row.get(name)
            if isinstance(value, int) and not isinstance(value, bool) and 0 <= value < (1 << 64):
                counters[name] = max(counters[name], value)
            else:
                malformed_controls += 1
    totals = {name: sum(value[name] for value in losses.values()) for name in counter_names}
    limited = any(row.get("event") == "log-limit" or (row.get("event") == "log-end" and row.get("reason") == "limit") for row in events)
    unclosed = started_sessions - closed_sessions
    observed_sessions = {session for row in rows if (session := writer_session(row)) is not None}
    missing_start = observed_sessions - started_sessions
    log_sessions = observed_sessions | started_sessions | closed_sessions | {key for key in losses if key != "legacy-unknown-session"}
    unscoped = any(writer_session(row) is None for row in rows)
    mixed_legacy = bool(log_sessions and unscoped)
    reported_issues = bool(invalid_lines or malformed_controls or malformed_gates or malformed_gate_losses
                           or reported_gate_losses or limited or unclosed or missing_start or mixed_legacy or any(totals.values()))
    times = [value for row in rows if (value := local(row.get("utc"))) is not None]
    last_elapsed = rows[-1].get("elapsedMs") if rows else None
    displays = collections.Counter()
    for row in samples:
        diagnostic = row.get("diagnostic", "")
        if not isinstance(diagnostic, str):
            diagnostic = ""
        if not row.get("pid"):
            displays["no-process"] += 1
        elif row.get("minimized"):
            displays["minimized"] += 1
        elif "无可见主窗口" in diagnostic:
            displays["hidden-not-proven-tray"] += 1
        else:
            displays["visible-by-adapter-diagnostic"] += 1
    return {
        "file": path.name,
        "sha256": hashlib.sha256(original).hexdigest(),
        "startShanghai": times[0] if times else None,
        "endShanghai": times[-1] if times else None,
        "completed": any(row.get("event") == "probe-end" for row in events),
        "durationMs": last_elapsed if numeric(last_elapsed) and len(log_sessions) == 1 and not unscoped and not missing_start and not unclosed else None,
        "lastRecordedElapsedMs": last_elapsed if numeric(last_elapsed) else None,
        "logIntegrity": {
            **totals,
            "limitReported": limited,
            "invalidLineCount": len(invalid_lines),
            "firstInvalidLines": invalid_lines[:16],
            "malformedControlCounterCount": malformed_controls,
            "malformedLyricGateEventCount": malformed_gates,
            "malformedLyricGateLossMarkerCount": malformed_gate_losses,
            "reportedLyricGatePreWriterDrops": reported_gate_losses,
            "writerSessionCount": len(log_sessions),
            "unclosedWriterSessionCount": len(unclosed),
            "writerSessionsMissingStartCount": len(missing_start),
            "multiSession": len(log_sessions) > 1,
            "hasUnscopedRecords": unscoped,
            "mixedLegacyAndScopedRecords": mixed_legacy,
            "reportedIssues": reported_issues,
            "observationCoverage": "incomplete" if reported_issues else "unknown",
            "allExpectedObservationsCaptured": None,
        },
        "sampleCount": len(samples),
        "appVersions": sorted({text(row, "appVersion", text(row, "prototypeVersion", "0.1.0-untagged")) for row in samples}),
        "clientVersions": sorted({text(row, "clientVersion") for row in samples}),
        "reliabilityCounts": dict(collections.Counter(text(row, "reliability", "unknown") for row in samples)),
        "stateCounts": dict(collections.Counter(text(row, "state", "unknown") for row in samples)),
        "displayCategories": dict(displays),
        "nonzeroDurationSamples": sum(numeric(row.get("durationMs")) and row["durationMs"] > 0 for row in samples),
        "audioActiveSamples": sum(audio(row).get("active") is True for row in samples),
        "nonzeroAudioSamples": sum(numeric(audio(row).get("rms")) and audio(row)["rms"] > 0 for row in samples),
        "maxRms": max((audio(row)["rms"] for row in samples if numeric(audio(row).get("rms"))), default=0),
        "internalHintTitleSamples": sum(isinstance(row.get("title"), str) and "SystemHint" in row["title"] for row in samples),
        "lyricCommitGates": {
            "observedEventCount": len(gate_events),
            "recognizedEventCount": len(recognized_gates),
            "malformedEventCount": malformed_gates,
            "reportedPreWriterDrops": reported_gate_losses,
            "malformedLossMarkerCount": malformed_gate_losses,
            "decisionCounts": dict(collections.Counter(row["decision"] for row in recognized_gates)),
            "reasonCounts": dict(collections.Counter(row["reason"] for row in recognized_gates)),
            "stageCounts": dict(collections.Counter(row["stage"] for row in recognized_gates)),
            "waitUpdatesCoalesced": True if recognized_gates else None,
            "allDecisionsCaptured": None,
            "displayConfirmed": None,
            "interpretation": "Counts are observed gate events, not unique requests or successful displays. eligible only passed the applicable request policy, including independent-preview and unverified-metadata branches; it does not confirm current playback. Wait events are coalesced. Logging, commit, rendering, matching and sync accuracy are separate evidence.",
        },
        "referenceClockCaptured": False,
        "syncAccuracyMs": None,
        "driftMs": None,
        "interpretation": "Observation summary only. completed means a probe-end event was recorded, not complete logging or acceptance. Loss/limit/bad lines/unclosed sessions make coverage incomplete; absent loss reports or a writer-end marker do not prove durable storage or all expected samples. Audio presence is not source-isolation acceptance; display categories are adapter diagnostics. Missing reference clock means no accuracy or drift measurement.",
    }

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    report = {"generatedUtc": datetime.datetime.now(datetime.timezone.utc).isoformat(), "observations": [summarize(path) for path in args.logs]}
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))
