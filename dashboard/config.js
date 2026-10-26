/* AGRIFLOW cloud dashboard — Firebase config.
 *
 * Project: agriflow (agriflow-iot), Spark (free) plan.
 * Realtime Database: https://agriflow-iot-default-rtdb.firebaseio.com (us-central1).
 *
 * NOTE: the database is in TEST MODE (open read/write) and those rules
 * EXPIRE on 2026-11-24 — after that date Firebase blocks all access until
 * the rules are updated. Before the expiry, either extend test mode in the
 * console or switch to locked rules (the ESP32 + dashboard would then need
 * auth — see README.md). Do NOT commit real keys to a public repo.
 */
const FIREBASE_CONFIG = {
  apiKey: "AIzaSyBqajOESNqmYvhVJxRssEo61zCH6zJ7Fbk",
  authDomain: "agriflow-iot.firebaseapp.com",
  databaseURL: "https://agriflow-iot-default-rtdb.firebaseio.com",
  projectId: "agriflow-iot",
  storageBucket: "agriflow-iot.firebasestorage.app",
  messagingSenderId: "1011223759942",
  appId: "1:1011223759942:web:f03fb49767be91cb644e47"
};
