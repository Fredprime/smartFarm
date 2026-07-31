# ⚡ Supabase Setup & Administration Guide

Your SmartFarm IoT dashboard is now integrated with Supabase for **Secure Authentication** and **Cloud Data Logging**.

---

## 1. Retrieve your Supabase Anon (Public) Key

1. Go to your Supabase project dashboard:
   👉 **[https://supabase.com/dashboard/project/exnhqpzlkucjiubvsabx](https://supabase.com/dashboard/project/exnhqpzlkucjiubvsabx)**
2. In the left menu, click **Project Settings ⚙️** → **API**.
3. Under **Project API keys**, find the key labeled `anon` `public`.
4. Click **Copy**.
5. Open [`dashboard/supabase_config.js`](file:///home/prime/Projects/smart_farm/dashboard/supabase_config.js) and paste the key into `anonKey`:

```javascript
const SUPABASE_CONFIG = {
  url: "https://exnhqpzlkucjiubvsabx.supabase.co",
  anonKey: "PASTE_YOUR_COPIED_ANON_KEY_HERE",
  cloudLogIntervalMs: 60000
};
```

---

## 2. Set Up Database Tables & Security Policies

1. In your Supabase dashboard, click **SQL Editor** (`</>`) in the left sidebar.
2. Click **New query**.
3. Copy the entire contents of [`docs/supabase_schema.sql`](file:///home/prime/Projects/smart_farm/docs/supabase_schema.sql) and paste it into the editor.
4. Click **Run** (or `Ctrl + Enter`).
5. You will see `Success. No rows returned`. This creates 3 tables:
   - `sensor_readings` (stores telemetry data every 60s)
   - `pump_events` (stores pump start/stop history)
   - `alerts_log` (stores active alert snapshots)

---

## 3. Create your Admin User

1. In your Supabase dashboard, click **Authentication 🔒** → **Users** in the left sidebar.
2. Click **Add user** → **Create user**.
3. Enter your admin email (e.g. `admin@smartfarm.com`) and a strong password.
4. Ensure **Auto Confirm User?** is checked/enabled.
5. Click **Create User**.

---

## 4. Test Login & Controller Access

1. Open [`dashboard/login.html`](file:///home/prime/Projects/smart_farm/dashboard/login.html) in your browser.
2. Sign in using the admin email and password created in Step 3.
3. Upon success, you will be redirected to the main SCADA dashboard [`dashboard/index.html`](file:///home/prime/Projects/smart_farm/dashboard/index.html).
4. Unauthenticated users visiting `index.html` directly will be automatically redirected back to `login.html`.
5. Click the **Logout 🔒** button in the top header to end your session.

---

## Database Architecture Overview

```
Browser Dashboard (login.html / index.html)
        │
        ├── Auth Session Check ──→ Supabase Auth (admin user)
        │
        ├── Telemetry Feed ─────→ sensor_readings table (logged every 60s)
        ├── Pump Activity ─────→ pump_events table (logged on ON/OFF)
        └── Critical Alerts ────→ alerts_log table (logged on alert changes)
```
