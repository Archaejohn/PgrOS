/* PgrOS portal client.
   Vanilla JS, no build step, no framework -- this has to fit on a 3.375 MiB
   partition alongside chat history, and there is no CDN reachable from the
   pager's access point. */

(function () {
  'use strict';

  var $ = function (id) { return document.getElementById(id); };

  /* ── Tabs ─────────────────────────────────────────── */
  var tabs = document.querySelectorAll('.tab');
  Array.prototype.forEach.call(tabs, function (t) {
    t.addEventListener('click', function () {
      Array.prototype.forEach.call(tabs, function (x) { x.classList.remove('is-active'); });
      t.classList.add('is-active');
      document.querySelectorAll('.view').forEach(function (v) { v.classList.remove('is-active'); });
      $('view-' + t.dataset.view).classList.add('is-active');
      if (t.dataset.view === 'gallery') loadGallery();
      if (t.dataset.view === 'track') loadTrack();
    });
  });

  /* ── Nickname, remembered per device ──────────────── */
  var nick = $('nick');
  try { nick.value = localStorage.getItem('pgros.nick') || ''; } catch (e) {}
  nick.addEventListener('change', function () {
    try { localStorage.setItem('pgros.nick', nick.value); } catch (e) {}
  });

  /* ── Chatroom ─────────────────────────────────────── */
  var lastId = 0;
  var box = $('messages');
  var pollTimer = null;

  function timeStr(ms) {
    // The device reports uptime-relative ms, not wall clock -- it may have no
    // RTC sync. Render relative, which is honest and needs no timezone.
    var age = Math.max(0, (Date.now() - startedAt) - (ms - firstDeviceMs));
    var s = Math.floor(age / 1000);
    if (s < 60) return 'now';
    if (s < 3600) return Math.floor(s / 60) + 'm';
    return Math.floor(s / 3600) + 'h';
  }
  var startedAt = Date.now();
  var firstDeviceMs = null;

  function render(msgs) {
    if (!msgs.length) return;
    var atBottom = (box.scrollHeight - box.scrollTop - box.clientHeight) < 80;
    var placeholder = box.querySelector('.empty');
    if (placeholder) placeholder.remove();

    msgs.forEach(function (m) {
      if (firstDeviceMs === null) firstDeviceMs = m.at;
      var el = document.createElement('div');
      el.className = 'msg' + (m.nick === nick.value && !m.mesh ? ' me' : '') + (m.mesh ? ' mesh' : '');
      var who = document.createElement('div');
      who.className = 'who';
      who.textContent = m.mesh ? (m.nick + ' · mesh') : m.nick;
      var body = document.createElement('div');
      body.className = 'body';
      body.textContent = m.text;
      var t = document.createElement('div');
      t.className = 'time';
      t.textContent = timeStr(m.at);
      el.appendChild(who); el.appendChild(body); el.appendChild(t);
      box.appendChild(el);
      lastId = Math.max(lastId, m.id);
    });

    if (atBottom) box.scrollTop = box.scrollHeight;
  }

  function poll() {
    fetch('/api/room?since=' + lastId, { cache: 'no-store' })
      .then(function (r) { return r.json(); })
      .then(function (d) { render(d.messages || []); })
      .catch(function () { /* AP dropped; the next tick retries */ })
      .then(function () { pollTimer = setTimeout(poll, 1500); });
  }
  poll();

  $('composer').addEventListener('submit', function (e) {
    e.preventDefault();
    var text = $('text').value.trim();
    if (!text) return;
    var body = 'nick=' + encodeURIComponent(nick.value || 'guest') +
               '&text=' + encodeURIComponent(text);
    $('text').value = '';
    fetch('/api/room', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: body
    }).then(function () {
      if (pollTimer) clearTimeout(pollTimer);
      poll();
    });
  });

  /* ── Gallery ──────────────────────────────────────── */
  var grid = $('grid');
  var maxBytes = 512 * 1024;

  function human(b) {
    if (b > 1048576) return (b / 1048576).toFixed(1) + ' MB';
    if (b > 1024) return Math.round(b / 1024) + ' KB';
    return b + ' B';
  }

  function loadGallery() {
    fetch('/api/gallery', { cache: 'no-store' })
      .then(function (r) { return r.json(); })
      .then(function (d) {
        maxBytes = d.max || maxBytes;
        $('quota').textContent = human(d.free) + ' free';
        grid.innerHTML = '';
        if (!d.items || !d.items.length) {
          grid.innerHTML = '<p class="empty">No photos yet.</p>';
          return;
        }
        d.items.forEach(function (it) {
          var cell = document.createElement('div');
          cell.className = 'cell';
          var img = document.createElement('img');
          img.loading = 'lazy';
          // Grid shows the thumbnail; the viewer fetches the full image only
          // when someone actually taps it.
          img.src = '/photo/' + (it.thumb ? thumbName(it.name) : it.name);
          img.addEventListener('click', function () {
            $('viewerImg').src = '/photo/' + it.name;
            $('viewer').hidden = false;
          });
          var del = document.createElement('button');
          del.className = 'del';
          del.textContent = '✕';
          del.addEventListener('click', function () {
            if (!confirm('Delete this photo?')) return;
            fetch('/api/gallery?name=' + encodeURIComponent(it.name), { method: 'DELETE' })
              .then(loadGallery);
          });
          cell.appendChild(img); cell.appendChild(del);
          grid.appendChild(cell);
        });
      })
      .catch(function () {
        grid.innerHTML = '<p class="empty">Could not reach the pager.</p>';
      });
  }

  $('viewerClose').addEventListener('click', function () {
    $('viewer').hidden = true;
    $('viewerImg').src = '';
  });

  /* Downscale before upload, and produce a thumbnail while we already have the
     image decoded.

     A modern phone photo is 3-5 MB; the device cap is 512 KB and the whole
     partition is a few megabytes, so resizing here is the difference between
     this working and every upload being rejected.

     The thumbnail matters for a different reason: the pager serves the gallery
     over its own access point, and sending twenty full-size images to fill a
     grid of 100px squares is painfully slow. The ESP32 has no business decoding
     JPEG to make thumbnails itself, so the browser does it -- once, off the same
     decode as the full image. */
  function renderTo(img, maxDim, maxBytes, quality) {
    var w = img.width, h = img.height;
    if (w > maxDim || h > maxDim) {
      if (w > h) { h = Math.round(h * maxDim / w); w = maxDim; }
      else { w = Math.round(w * maxDim / h); h = maxDim; }
    }
    var c = document.createElement('canvas');
    c.width = w; c.height = h;
    c.getContext('2d').drawImage(img, 0, 0, w, h);

    return new Promise(function (resolve) {
      // Step the quality down until it fits, rather than failing outright.
      (function attempt(q) {
        c.toBlob(function (blob) {
          if (!blob) { resolve(null); return; }
          if (blob.size <= maxBytes || q <= 0.35) { resolve(blob); return; }
          attempt(q - 0.15);
        }, 'image/jpeg', q);
      })(quality);
    });
  }

  function prepare(file) {
    return new Promise(function (resolve) {
      var img = new Image();
      var url = URL.createObjectURL(file);
      img.onload = function () {
        URL.revokeObjectURL(url);
        Promise.all([
          renderTo(img, 1280, maxBytes, 0.82),
          // 320px covers a 2x display of the ~104px grid cell.
          renderTo(img, 320, 24 * 1024, 0.7)
        ]).then(function (parts) {
          resolve({ full: parts[0] || file, thumb: parts[1] });
        });
      };
      img.onerror = function () {
        URL.revokeObjectURL(url);
        resolve({ full: file, thumb: null });
      };
      img.src = url;
    });
  }

  /* "a1b2c3d4.jpg" -> "a1b2c3d4t.jpg". Mirrors thumbPathFor() on the device. */
  function thumbName(name) {
    return name.slice(0, 8) + 't' + name.slice(8);
  }

  $('file').addEventListener('change', function (e) {
    var files = Array.prototype.slice.call(e.target.files);
    if (!files.length) return;
    e.target.value = '';

    var bar = $('progressBar');
    $('progress').hidden = false;
    var done = 0;

    (function next() {
      if (!files.length) {
        $('progress').hidden = true;
        bar.style.width = '0';
        loadGallery();
        return;
      }
      var f = files.shift();
      prepare(f).then(function (parts) {
        var stem = (f.name || 'photo').replace(/\.[^.]+$/, '');
        var fd = new FormData();
        fd.append('photo', parts.full, stem + '.jpg');
        // Both halves go in one request so they share an id on the device and
        // cannot end up orphaned by a dropped second request.
        if (parts.thumb) fd.append('thumb', parts.thumb, stem + '.jpg');
        return fetch('/api/upload', { method: 'POST', body: fd });
      }).then(function (r) {
        if (r && !r.ok) console.warn('upload rejected', r.status);
      }).catch(function () {
        /* keep going; one bad photo should not abort the batch */
      }).then(function () {
        done++;
        bar.style.width = Math.round(done * 100 / (done + files.length)) + '%';
        next();
      });
    
  /* ── Track ────────────────────────────────────────── */

  function loadTrack() {
    fetch('/api/track', { cache: 'no-store' })
      .then(function (r) { return r.json(); })
      .then(function (d) {
        if (!d.points) {
          $('trackSummary').innerHTML =
            '<div><b>0</b><span>points</span></div>' +
            '<div><span>Enable "Record GPS track" in Settings &rsaquo; Privacy</span></div>';
          $('trackDl').style.opacity = .4;
          return;
        }
        $('trackDl').style.opacity = 1;

        var km = d.metres >= 1000
          ? (d.metres / 1000).toFixed(2) + ' km'
          : d.metres + ' m';

        // Worst bars is the headline: it is the answer to "did I lose the mesh".
        var worst = d.worstBars + '/4';

        $('trackSummary').innerHTML =
          '<div><b>' + d.points.toLocaleString() + '</b><span>points</span></div>' +
          '<div><b>' + km + '</b><span>distance</span></div>' +
          '<div><b>' + worst + '</b><span>weakest mesh</span></div>' +
          '<div><b>' + Math.round(d.bytes / 1024) + ' KB</b><span>on device</span></div>' +
          (d.recording ? '<div><b>REC</b><span>recording</span></div>' : '');

        loadMap();
      })
      .catch(function () {
        $('trackSummary').textContent = 'Could not reach the pager.';
      });
  }


  /* ── Map ──────────────────────────────────────────────
     Leaflet and the tiles are fetched by the BROWSER from the internet, not by
     the pager. The device stores no map data and serves none -- it only hands
     over its own track. The consequence is that the map works when the pager is
     joined to a WiFi network (so the phone still has a route out) and not when
     the phone is attached to the pager's own hotspot, which has no uplink. The
     GPX download works either way, so that is the fallback. */
  var CDN = 'https://unpkg.com/leaflet@1.9.4/dist/';
  var leafletState = 'idle'; // idle | loading | ready | failed
  var map = null, trackLayer = null;

  function barColour(bars) {
    return ['#e0524a', '#e08b3c', '#d8c53c', '#8bc34a', '#3ec46d'][Math.max(0, Math.min(4, bars))];
  }

  function loadLeaflet() {
    if (leafletState === 'ready' || leafletState === 'loading') return Promise.resolve(leafletState === 'ready');
    leafletState = 'loading';

    return new Promise(function (resolve) {
      var css = document.createElement('link');
      css.rel = 'stylesheet';
      css.href = CDN + 'leaflet.css';
      document.head.appendChild(css);

      var js = document.createElement('script');
      js.src = CDN + 'leaflet.js';

      // No internet means onerror, but some captive setups just hang instead,
      // so cap the wait rather than leaving the user staring at nothing.
      var settled = false;
      var done = function (ok) {
        if (settled) return;
        settled = true;
        leafletState = ok ? 'ready' : 'failed';
        resolve(ok);
      };
      js.onload = function () { done(true); };
      js.onerror = function () { done(false); };
      setTimeout(function () { done(typeof window.L !== 'undefined'); }, 6000);

      document.head.appendChild(js);
    });
  }

  function drawTrack(pts) {
    if (!map) {
      map = L.map('map', { attributionControl: true });
      L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png', {
        maxZoom: 19,
        attribution: '&copy; OpenStreetMap contributors'
      }).addTo(map);
    }
    if (trackLayer) { map.removeLayer(trackLayer); }
    trackLayer = L.layerGroup().addTo(map);

    // One polyline per consecutive run of equal signal, so the line itself
    // shows where the mesh dropped off rather than needing a separate overlay.
    var bounds = [];
    for (var i = 0; i < pts.length - 1; i++) {
      var a = pts[i], b = pts[i + 1];
      bounds.push([a[0], a[1]]);
      L.polyline([[a[0], a[1]], [b[0], b[1]]], {
        color: barColour(a[3]), weight: 5, opacity: 0.9
      }).addTo(trackLayer);
    }
    if (pts.length) bounds.push([pts[pts.length - 1][0], pts[pts.length - 1][1]]);

    if (pts.length) {
      var first = pts[0], last = pts[pts.length - 1];
      L.circleMarker([first[0], first[1]], { radius: 6, color: '#fff', weight: 2, fillColor: '#3ec46d', fillOpacity: 1 })
        .bindPopup('Start').addTo(trackLayer);
      L.circleMarker([last[0], last[1]], { radius: 6, color: '#fff', weight: 2, fillColor: '#6e9eff', fillOpacity: 1 })
        .bindPopup('End').addTo(trackLayer);
    }

    if (bounds.length) map.fitBounds(bounds, { padding: [20, 20] });
    setTimeout(function () { map.invalidateSize(); }, 50);
  }


  /* ── Coverage surface (IDW) ───────────────────────────
     Kriging was the other candidate and is not viable here: it solves an n x n
     system, so 1500 points is ~1e9 operations plus variogram fitting. It would
     need decimating to a couple of hundred points to run at all, which throws
     away the resolution that made it worth doing.

     IDW is cheap ENOUGH, but only with a cutoff radius and a spatial index.
     Naive IDW is cells x points -- 40k cells against 1500 points is 60M distance
     calculations. Bucketing the samples at the search radius means each cell
     only ever looks at its nine neighbouring buckets, which is ~1M operations
     and comfortably inside a frame.

     The important part is not performance though. This data is a PATH, not a
     scattered sample field, so any interpolator will happily paint confident
     colour hundreds of metres off-track where nothing was ever measured. So
     cells with no sample inside the radius are left fully transparent: no
     reading is shown as no data, never as bad coverage. */
  var IDW_RADIUS_M = 300;   // how far a sample is allowed to speak for
  var IDW_CELL_PX = 5;      // output resolution; smaller is slower, not better
  var IDW_POWER = 2;        // classic inverse-square weighting

  var HeatLayer = null;

  function defineHeatLayer() {
    if (HeatLayer) return;
    HeatLayer = L.Layer.extend({
      initialize: function (pts) { this._pts = pts; },

      onAdd: function (map) {
        this._map = map;
        this._canvas = L.DomUtil.create('canvas', 'leaflet-zoom-animated');
        map.getPanes().overlayPane.appendChild(this._canvas);
        map.on('moveend zoomend resize', this._redraw, this);
        this._redraw();
      },

      onRemove: function (map) {
        map.off('moveend zoomend resize', this._redraw, this);
        if (this._canvas && this._canvas.parentNode) this._canvas.parentNode.removeChild(this._canvas);
      },

      _redraw: function () {
        var map = this._map, pts = this._pts;
        if (!map || !pts.length) return;

        var size = map.getSize();
        var cv = this._canvas;
        cv.width = size.x; cv.height = size.y;
        cv.style.width = size.x + 'px'; cv.style.height = size.y + 'px';
        L.DomUtil.setPosition(cv, map.containerPointToLayerPoint([0, 0]));

        // Radius is a real-world distance, so it has to be converted to pixels
        // at the current zoom -- otherwise the surface would grow and shrink as
        // you zoom, which is meaningless.
        var c = map.getCenter();
        var mPerPx = 40075016.686 * Math.cos(c.lat * Math.PI / 180) / Math.pow(2, map.getZoom() + 8);
        var R = Math.max(10, Math.min(220, IDW_RADIUS_M / mPerPx));
        var R2 = R * R;

        // Project once, and keep only what can affect the viewport.
        var pad = R + IDW_CELL_PX;
        var proj = [];
        for (var i = 0; i < pts.length; i++) {
          var p = map.latLngToContainerPoint([pts[i][0], pts[i][1]]);
          if (p.x < -pad || p.y < -pad || p.x > size.x + pad || p.y > size.y + pad) continue;
          proj.push([p.x, p.y, pts[i][3]]);
        }
        var ctx = cv.getContext('2d');
        ctx.clearRect(0, 0, size.x, size.y);
        if (!proj.length) return;

        // Bucket at the search radius: each output cell then only consults the
        // 3x3 block around it instead of every sample.
        var bw = Math.ceil((size.x + 2 * pad) / R) + 1;
        var bh = Math.ceil((size.y + 2 * pad) / R) + 1;
        var buckets = new Array(bw * bh);
        for (i = 0; i < proj.length; i++) {
          var bx = Math.floor((proj[i][0] + pad) / R);
          var by = Math.floor((proj[i][1] + pad) / R);
          var bi = by * bw + bx;
          (buckets[bi] || (buckets[bi] = [])).push(proj[i]);
        }

        var img = ctx.createImageData(size.x, size.y);
        var data = img.data;

        for (var cy = 0; cy < size.y; cy += IDW_CELL_PX) {
          for (var cx = 0; cx < size.x; cx += IDW_CELL_PX) {
            var px = cx + IDW_CELL_PX / 2, py = cy + IDW_CELL_PX / 2;
            var gx = Math.floor((px + pad) / R), gy = Math.floor((py + pad) / R);

            var wsum = 0, vsum = 0, near = false;
            for (var oy = -1; oy <= 1; oy++) {
              for (var ox = -1; ox <= 1; ox++) {
                var b = buckets[(gy + oy) * bw + (gx + ox)];
                if (!b) continue;
                for (var k = 0; k < b.length; k++) {
                  var dx = b[k][0] - px, dy = b[k][1] - py;
                  var d2 = dx * dx + dy * dy;
                  if (d2 > R2) continue;
                  near = true;
                  if (d2 < 1) d2 = 1;             // avoid a singularity on top of a sample
                  var w = 1 / Math.pow(d2, IDW_POWER / 2);
                  wsum += w; vsum += w * b[k][2];
                }
              }
            }

            // No sample within the radius means no information. Leave it clear
            // rather than inventing coverage.
            if (!near || wsum === 0) continue;

            var val = vsum / wsum;                 // 0..4 bars
            var col = barColour(Math.round(val));
            var r = parseInt(col.substr(1, 2), 16),
                g = parseInt(col.substr(3, 2), 16),
                bl = parseInt(col.substr(5, 2), 16);

            for (var yy = cy; yy < cy + IDW_CELL_PX && yy < size.y; yy++) {
              var row = yy * size.x;
              for (var xx = cx; xx < cx + IDW_CELL_PX && xx < size.x; xx++) {
                var o = (row + xx) * 4;
                data[o] = r; data[o + 1] = g; data[o + 2] = bl; data[o + 3] = 150;
              }
            }
          }
        }
        ctx.putImageData(img, 0, 0);
      }
    });
  }

  var heatLayer = null;
  var mapMode = 'line';
  var lastPoints = [];

  function applyMapMode() {
    if (!map) return;
    $('modeLine').classList.toggle('is-on', mapMode === 'line');
    $('modeHeat').classList.toggle('is-on', mapMode === 'heat');

    if (trackLayer) map.removeLayer(trackLayer);
    if (heatLayer) { map.removeLayer(heatLayer); heatLayer = null; }

    if (mapMode === 'line') {
      if (trackLayer) trackLayer.addTo(map);
    } else {
      defineHeatLayer();
      heatLayer = new HeatLayer(lastPoints);
      heatLayer.addTo(map);
    }
  }

  $('modeLine').addEventListener('click', function () { mapMode = 'line'; applyMapMode(); });
  $('modeHeat').addEventListener('click', function () { mapMode = 'heat'; applyMapMode(); });

  function loadMap() {
    var note = $('mapNote');
    var el = $('map');

    fetch('/api/track.json', { cache: 'no-store' })
      .then(function (r) { return r.json(); })
      .then(function (d) {
        if (!d.points || !d.points.length) {
          el.classList.add('is-hidden');
          $('mapLegend').style.display = 'none';
          note.textContent = '';
          return;
        }
        note.textContent = 'Loading map…';
        return loadLeaflet().then(function (ok) {
          if (!ok) {
            el.classList.add('is-hidden');
            $('mapLegend').style.display = 'none';
            note.innerHTML = 'Map unavailable &mdash; this device has no internet connection. ' +
              'It works when the pager is joined to a WiFi network instead of running its own hotspot. ' +
              'Download the GPX above to view the track elsewhere.';
            return;
          }
          el.classList.remove('is-hidden');
          $('mapLegend').style.display = '';
          note.textContent = d.step > 1
            ? ('Showing every ' + d.step + 'th point of ' + d.total.toLocaleString() + '.')
            : '';
          lastPoints = d.points;
          drawTrack(d.points);
          applyMapMode();
        });
      })
      .catch(function () { note.textContent = 'Could not load the track.'; });
  }

  $('trackClear').addEventListener('click', function () {
    if (!confirm('Delete the recorded track?')) return;
    fetch('/api/track', { method: 'DELETE' }).then(loadTrack);
  });
})();
  });
})();
