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
    })();
  });
})();
