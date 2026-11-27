const CACHE_NAME = "medroute-v6";
const ASSETS = ["/", "/admin", "/manifest.json", "/icon.svg"];

self.addEventListener("install", event => {
  event.waitUntil(caches.open(CACHE_NAME).then(cache => cache.addAll(ASSETS)));
  self.skipWaiting();
});

self.addEventListener("fetch", event => {
  if (event.request.method !== "GET" || event.request.url.includes("/api/")) return;
  event.respondWith(
    fetch(event.request).catch(() => caches.match(event.request))
  );
});