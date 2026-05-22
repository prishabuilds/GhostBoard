-- ╔══════════════════════════════════════════════════════════╗
-- ║         GhostBoard — Supabase Database Schema           ║
-- ║  Run this entire file in Supabase → SQL Editor → Run   ║
-- ╚══════════════════════════════════════════════════════════╝

-- ── 1. Sensor telemetry (one row per reading) ──────────────────
CREATE TABLE IF NOT EXISTS sensor_data (
    id              BIGSERIAL PRIMARY KEY,
    created_at      TIMESTAMPTZ DEFAULT NOW(),
    temperature     FLOAT       NOT NULL,
    data_valid      BOOLEAN     DEFAULT TRUE,
    system_state    SMALLINT    DEFAULT 0,   -- 0=HEALTHY 1=NERVOUS 2=CRITICAL 3=RECOVERING
    free_heap       INTEGER,
    uptime_ms       BIGINT
);

-- ── 2. Log entries (mirrors LogEntry_t from firmware) ─────────
CREATE TABLE IF NOT EXISTS logs (
    id              BIGSERIAL PRIMARY KEY,
    created_at      TIMESTAMPTZ DEFAULT NOW(),
    timestamp_ms    BIGINT      NOT NULL,
    source          TEXT        NOT NULL,    -- task name
    severity        SMALLINT    NOT NULL,    -- 0=DEBUG .. 4=CRITICAL
    message         TEXT        NOT NULL
);

-- ── 3. Task heartbeat status (upsert by task name) ────────────
CREATE TABLE IF NOT EXISTS task_status (
    task_name       TEXT PRIMARY KEY,
    last_heartbeat  TIMESTAMPTZ DEFAULT NOW(),
    is_healthy      BOOLEAN     DEFAULT TRUE,
    miss_count      SMALLINT    DEFAULT 0,
    stack_hwm       INTEGER,
    retry_count     SMALLINT    DEFAULT 0
);

-- ── 4. System state history ───────────────────────────────────
CREATE TABLE IF NOT EXISTS state_history (
    id              BIGSERIAL PRIMARY KEY,
    created_at      TIMESTAMPTZ DEFAULT NOW(),
    from_state      SMALLINT,
    to_state        SMALLINT,
    trigger_reason  TEXT
);

-- ── Enable Realtime on all tables ────────────────────────────
ALTER PUBLICATION supabase_realtime ADD TABLE sensor_data;
ALTER PUBLICATION supabase_realtime ADD TABLE logs;
ALTER PUBLICATION supabase_realtime ADD TABLE task_status;
ALTER PUBLICATION supabase_realtime ADD TABLE state_history;

-- ── Row Level Security — allow anon INSERT/SELECT ────────────
-- (for production, use service_role key on ESP32 only)
ALTER TABLE sensor_data   ENABLE ROW LEVEL SECURITY;
ALTER TABLE logs          ENABLE ROW LEVEL SECURITY;
ALTER TABLE task_status   ENABLE ROW LEVEL SECURITY;
ALTER TABLE state_history ENABLE ROW LEVEL SECURITY;

CREATE POLICY "allow_all_sensor"  ON sensor_data   FOR ALL USING (true) WITH CHECK (true);
CREATE POLICY "allow_all_logs"    ON logs          FOR ALL USING (true) WITH CHECK (true);
CREATE POLICY "allow_all_tasks"   ON task_status   FOR ALL USING (true) WITH CHECK (true);
CREATE POLICY "allow_all_history" ON state_history FOR ALL USING (true) WITH CHECK (true);

-- ── Seed initial task rows (so dashboard shows them on first load)
INSERT INTO task_status (task_name, is_healthy, miss_count, retry_count) VALUES
    ('HealthMonitor', true, 0, 0),
    ('RecoveryMgr',   true, 0, 0),
    ('SensorTask',    true, 0, 0),
    ('CommTask',      true, 0, 0),
    ('LoggingTask',   true, 0, 0),
    ('OledUITask',    true, 0, 0)
ON CONFLICT (task_name) DO NOTHING;
