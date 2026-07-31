// ============================================================
//  supabase_config.js — Supabase Connection Configuration
//  Project: SmartFarm IoT
// ============================================================

const SUPABASE_CONFIG = {
  // Your Supabase Project URL
  url: "https://exnhqpzlkucjiubvsabx.supabase.co",

  // Your Supabase Anon (Public) Key
  // Copy this from: Supabase Dashboard -> Project Settings -> API -> Project API keys -> anon / public
  anonKey: "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImV4bmhxcHpsa3Vjaml1YnZzYWJ4Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3ODU0OTgxOTYsImV4cCI6MjEwMTA3NDE5Nn0.LQFvw98M2cN4Ojf7LoUp2kJR7bMoCGICcdSmM9xlW9g",

  // Sensor reading cloud log interval (ms) — default: 60,000ms (1 minute)
  cloudLogIntervalMs: 60000
};

// Global Supabase client instance
let supabaseClient = null;

function initSupabase() {
  if (typeof supabase !== 'undefined' && SUPABASE_CONFIG.url && SUPABASE_CONFIG.anonKey && SUPABASE_CONFIG.anonKey !== "YOUR_SUPABASE_ANON_KEY") {
    try {
      supabaseClient = supabase.createClient(SUPABASE_CONFIG.url, SUPABASE_CONFIG.anonKey);
      console.log('[Supabase] Initialized successfully with project:', SUPABASE_CONFIG.url);
      return supabaseClient;
    } catch (err) {
      console.error('[Supabase] Initialization error:', err);
    }
  } else {
    console.warn('[Supabase] Anon Key missing or invalid. Cloud logging and remote Auth disabled until key is added in supabase_config.js');
  }
  return null;
}
