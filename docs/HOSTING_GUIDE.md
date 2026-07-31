# 🌐 Hosting SmartFarm IoT Online (Complete Guide)

There are **two parts** to hosting your SmartFarm system online:
1. **Hosting the Dashboard** (HTML / CSS / JS) so you can open it anywhere on your phone or laptop.
2. **Connecting to the ESP32** remotely when you are away from the local farm Wi-Fi network.

---

## Part 1: Hosting the Dashboard Online (Free & Instant)

Because the dashboard is built with standard HTML, CSS, and JavaScript, you can host it for free on any modern web host in less than 2 minutes.

### Option A: Netlify (Easiest — Drag & Drop)
1. Go to **[Netlify Drop](https://app.netlify.com/drop)**.
2. Drag and drop your local `smart_farm/dashboard` folder directly onto the browser window.
3. Netlify will generate a live HTTPS URL (e.g. `https://smartfarm-controller.netlify.app`).

### Option B: Vercel
1. Install Vercel CLI or sign up at **[vercel.com](https://vercel.com)**.
2. In your terminal inside `smart_farm/dashboard`:
   ```bash
   npx vercel
   ```
3. Follow the 30-second prompts to deploy.

### Option C: GitHub Pages
1. Push your repository to GitHub.
2. Go to **Repository Settings** → **Pages**.
3. Set Source to `main` branch and folder to `/dashboard`.
4. Your site will be live at `https://<your-username>.github.io/<repo-name>/`.

---

## Part 2: Connecting to your ESP32 Remotely

When you open your online dashboard outside your home/farm Wi-Fi, your browser needs a way to talk to the ESP32 at its IP (`10.74.192.38`). Here are the 3 best options:

### Method 1: Ngrok or Cloudflare Tunnel (Recommended & Safe)
If you have a computer or Raspberry Pi running on the farm Wi-Fi:
1. Install [ngrok](https://ngrok.com):
   ```bash
   ngrok http 10.74.192.38:81
   ```
2. Ngrok will give you a secure WebSocket address (e.g. `wss://abc1234.ngrok-free.app`).
3. In your dashboard **Settings** (or input bar), update the ESP32 IP/address to `abc1234.ngrok-free.app`.

### Method 2: Router Port Forwarding & Dynamic DNS
If your farm router has a public IP:
1. Access your Wi-Fi router settings page (`192.168.1.1` or `10.74.192.1`).
2. Go to **Port Forwarding**.
3. Forward External Port `81` to Internal IP `10.74.192.38`, Port `81`.
4. Use a free DDNS service like **[DuckDNS](https://www.duckdns.org)** to get a hostname (e.g., `myfarm.duckdns.org`).
5. Set your dashboard ESP32 IP to `myfarm.duckdns.org`.

### Method 3: Direct ESP32 Web Server (Built-In)
Your ESP32 firmware already has an HTTP and WebSocket server running on port `80` and `81`:
- Anyone on the local Wi-Fi can open `http://10.74.192.38` directly in their browser.
- If you forward port 80 on your router, you can access the dashboard directly from your ESP32 anywhere.
