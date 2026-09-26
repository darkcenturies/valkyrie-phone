"""Build valkyrie-web.dat, the phone's copy of GTA IV's internet, from the
player's own copy of GTA IV.

    python build-web-pack.py <GTA IV>\\pc\\html valkyrie-web.dat [--gxt <GTA IV>\\pc\\text\\american.gxt]
    python build-web-pack.py --sa-only valkyrie-web.dat
    python build-web-pack.py <GTA IV>\\pc\\html valkyrie-web.dat --with assets\\web\\valkyrie-web.dat

San Andreas' own sites go in as well - or on their own, with --sa-only, for
a player without GTA IV. The real ones Rockstar put up for the game in 2004
(the Epsilon Program, eXsorbeo, Cluckin' Bell - all Flash, played by the
archive's own Ruffle - and Maccer's) are fetched from the Internet Archive's
Wayback Machine as the pack is built, on the player's machine, and never kept
in this project; each is laid out at its own width and fitted to the
phone's, to be zoomed into. A site whose front page the archive will not
give, West Coast Rap Legends (never captured) and sp-rp.com are the pages in
tools/sa-web, written for this project.
--no-archive keeps to those. --with takes San Andreas' sites from a pack
already built instead (the one kept in assets\\web), so nothing is fetched and
only GTA IV's pages are laid out; build.ps1 builds the pack that way when it
finds GTA IV.

Each .whm page is turned into HTML (whm.py), reflowed for the phone's
narrow screen and laid out by Chrome at the phone's width (320 points, drawn
at 1.5 pixels a point so it stays sharp), and saved as a 256-colour PNG with
the rectangles of its links. --desktop keeps GTA IV's own 640-wide layout.
The pack holds nothing from GTA IV that is not in the player's own copy of
it; it is built on their machine and never shipped.

Needs Python 3, Pillow, Playwright (pip install pillow playwright) and
Google Chrome.

The pack, little-endian:
    "VWEB", u32 version (2), u32 page count, u32 link count,
    u32 offset of the links, u32 offset of the strings
    pages:   u32 key, u32 title (offsets into the strings), u16 width,
             u16 height, u32 PNG offset, u32 PNG size, u32 first link,
             u32 link count
    links:   i32 target page (-1 none, -2 back; -3 and below a link out to
             the real internet, its address the string at -(target) - 3),
             u16 x, y, w, h
    strings: NUL-terminated, cp1252
    then the PNGs.
"""
import argparse, glob, io, os, pathlib, re, shutil, struct, sys, tempfile

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import whm  # noqa: E402

LINKS_JS = """() => {
  const out = [];
  for (const a of document.querySelectorAll('a[data-href]')) {
    const href = a.getAttribute('data-href');
    if (!href) continue;
    const rects = [];
    const push = r => { if (r.width > 0 && r.height > 0) rects.push([r.left + scrollX, r.top + scrollY, r.width, r.height]); };
    for (const r of a.getClientRects()) push(r);
    for (const img of a.querySelectorAll('img')) push(img.getBoundingClientRect());
    if (rects.length) out.push({href, rects});
  }
  return {w: document.documentElement.scrollWidth, h: document.documentElement.scrollHeight, links: out};
}"""


def joaat(s):
    h = 0
    for c in s.lower().encode('latin1'):
        h = (h + c) & 0xFFFFFFFF
        h = (h + (h << 10)) & 0xFFFFFFFF
        h ^= h >> 6
    h = (h + (h << 3)) & 0xFFFFFFFF
    h ^= h >> 11
    return (h + (h << 15)) & 0xFFFFFFFF


class Gxt(dict):
    """GTA IV's text file: every table's entries by the hash of their key."""

    def __init__(self, path):
        super().__init__()
        d = open(path, 'rb').read()
        tabl = d.index(b'TABL')
        size = struct.unpack_from('<I', d, tabl + 4)[0]
        for i in range(size // 12):
            name, off = struct.unpack_from('<8sI', d, tabl + 8 + i * 12)
            name = name.rstrip(b'\0')
            p = off if name == b'MAIN' else off + 8
            if d[p:p + 4] != b'TKEY':
                continue
            ksize = struct.unpack_from('<I', d, p + 4)[0]
            dat = p + 8 + ksize
            if d[dat:dat + 4] != b'TDAT':
                continue
            for k in range(ksize // 8):
                toff, h = struct.unpack_from('<II', d, p + 8 + k * 8)
                s = dat + 8 + toff
                e = s
                while d[e:e + 2] != b'\0\0':
                    e += 2
                self[h] = d[s:e].decode('utf-16-le', 'replace')

    def get(self, key, default=''):
        return dict.get(self, joaat(key), default)


# The phone's screen is 320 points across.
MOBILE_WIDTH = 320
MOBILE_PIXEL_RATIO = 1.5


def convert(src, work, gxt, mobile):
    pages = []
    for path in sorted(glob.glob(os.path.join(src, '*', '*.whm'))):
        folder = os.path.basename(os.path.dirname(path))
        if ' ' in folder:  # "www.a-thousand-words.net - original": a leftover copy
            continue
        site = folder.lower()
        name = os.path.splitext(os.path.basename(path))[0].lower()
        out_dir = os.path.join(work, site)
        c = whm.Converter(whm.Page(path), site, name, os.path.join(out_dir, 'img'), 'img/', gxt, mobile)
        doc = c.convert()
        os.makedirs(out_dir, exist_ok=True)
        html_path = os.path.join(out_dir, name + '.html')
        open(html_path, 'w', encoding='utf-8').write(doc)
        pages.append((site + '/' + name, c.title, html_path))
    return pages


SA_WEB = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'sa-web')


def sa_pages():
    """San Andreas' own sites: sa-web/<site>/<page>.html, already written for
    the phone's width. Links are data-href="<site>/<page>" inside the pack
    and data-href="ext:<address>" out to the real internet."""
    pages = []
    for path in sorted(glob.glob(os.path.join(SA_WEB, '*', '*.html'))):
        site = os.path.basename(os.path.dirname(path)).lower()
        name = os.path.splitext(os.path.basename(path))[0].lower()
        text = open(path, encoding='utf-8').read()
        m = re.search(r'<title>(.*?)</title>', text, re.S)
        pages.append((site + '/' + name, m.group(1).strip() if m else site, path))
    return pages


# The real sites, as the Wayback Machine keeps them: (pack key, the address
# the page was at, the year to ask for). The domains framed pages kept at
# rockstargames.com in 2004; Maccer's own came later.
ARCHIVE = [
    ('www.epsilonprogram.com/index', 'www.rockstargames.com/epsilonprogram/index.htm', '2004'),
    ('www.epsilonprogram.com/join', 'www.rockstargames.com/epsilonprogram/join.htm', '2004'),
    ('www.epsilonprogram.com/money', 'www.rockstargames.com/epsilonprogram/money.htm', '2004'),
    ('www.epsilonprogram.com/testimonials', 'www.rockstargames.com/epsilonprogram/testimonials.htm', '2004'),
    ('www.epsilonprogram.com/tract', 'www.rockstargames.com/epsilonprogram/tract.htm', '2004'),
    ('www.epsilonprogram.com/contact', 'www.rockstargames.com/epsilonprogram/contact.htm', '2004'),
    ('www.exsorbeo.com/index', 'www.rockstargames.com/exsorbeo/index.htm', '2004'),
    ('www.exsorbeo.com/games', 'www.rockstargames.com/exsorbeo/games.htm', '2004'),
    ('www.exsorbeo.com/emulator', 'www.rockstargames.com/exsorbeo/emulator.htm', '2004'),
    ('www.exsorbeo.com/blindness', 'www.rockstargames.com/exsorbeo/blindness.htm', '2004'),
    ('www.cluckinbellhappychicken.com/index', 'www.rockstargames.com/cluckinbell/', '2004'),
    ('www.maccer.net/index', 'maccer.net/home.html', '2019'),
    ('www.maccer.net/biography', 'maccer.net/biography.html', '2019'),
    ('www.maccer.net/comebacktour', 'maccer.net/comeBackTour.html', '2019'),
    ('www.maccer.net/music', 'maccer.net/music.html', '2019'),
    ('www.maccer.net/personalmessage', 'maccer.net/personalMessage.html', '2019'),
    ('www.maccer.net/personalquotes', 'maccer.net/personalQuotes.html', '2019'),
    ('www.maccer.net/rumours', 'maccer.net/rumours.html', '2019'),
]
# Sites made all of Flash get longer to start playing before the picture.
FLASH_SITES = {'www.cluckinbellhappychicken.com'}
ARCHIVE_WIDTH = 960  # pixels the fitted page is kept at: twice the phone's width, for zooming

# A page from the archive: its links (plain ones and image-map areas - the
# 2004 sites' menus are mostly maps), each turned into the pack page it was
# where there is one, and the span of the page's own layout, which sits in
# the middle of a wider window. Addresses may come as the archive's or as the
# original's.
ARCHIVE_LINKS_JS = r"""(map) => {
  const key = (href) => {
    const m = href.match(/\/web\/\d+[a-z_]*\/(.*)$/i);
    let at = (m ? m[1] : href).toLowerCase();
    at = at.replace(/^https?:\/\//, '').replace(/:80\//, '/').replace(/^www\./, '').replace(/[?#].*$/, '');
    return map[at] || map[at.replace(/\/$/, '')] || null;
  };
  const links = [];
  for (const a of document.querySelectorAll('a[href]')) {
    const k = key(a.href);
    const rects = [...a.getClientRects()].filter(r => r.width > 1 && r.height > 1)
      .map(r => [r.left + scrollX, r.top + scrollY, r.width, r.height]);
    if (k && rects.length) links.push({href: k, rects});
  }
  for (const area of document.querySelectorAll('area[href]')) {
    const k = key(area.href);
    const mapEl = area.closest('map');
    if (!k || !mapEl) continue;
    const img = document.querySelector('img[usemap="#' + mapEl.name + '"], img[usemap="#' + mapEl.id + '"]');
    if (!img) continue;
    const box = img.getBoundingClientRect();
    // The coordinates are for the picture as the page drew it; a picture
    // shrunk for the phone's screen scales them with it.
    const was = Number(img.dataset.w0) || box.width;
    const f = was > 0 ? box.width / was : 1;
    const c = (area.getAttribute('coords') || '').split(',').map(Number).filter(v => !isNaN(v)).map(v => v * f);
    if (c.length < 3) continue;
    let x0, y0, x1, y1;
    if ((area.getAttribute('shape') || '').toLowerCase().startsWith('circ')) {
      x0 = c[0] - c[2]; y0 = c[1] - c[2]; x1 = c[0] + c[2]; y1 = c[1] + c[2];
    } else {
      const xs = c.filter((_, i) => i % 2 === 0), ys = c.filter((_, i) => i % 2 === 1);
      x0 = Math.min(...xs); x1 = Math.max(...xs); y0 = Math.min(...ys); y1 = Math.max(...ys);
    }
    links.push({href: k, rects: [[box.left + scrollX + x0, box.top + scrollY + y0, x1 - x0, y1 - y0]]});
  }
  // The layout's own span: what its tables and pictures cover.
  let left = Infinity, right = -Infinity;
  for (const el of document.querySelectorAll('table, img, embed, object, ruffle-player, ruffle-object, ruffle-embed')) {
    const r = el.getBoundingClientRect();
    if (r.width < 8 || r.height < 8 || r.width >= innerWidth - 4) continue;
    left = Math.min(left, r.left + scrollX); right = Math.max(right, r.right + scrollX);
  }
  const width = document.documentElement.scrollWidth;
  if (!(right - left > 200)) { left = 0; right = width; }
  return {links, left: Math.max(0, Math.floor(left)), right: Math.min(width, Math.ceil(right))};
}"""


# A 2004 page laid out again for the phone's narrow screen, as GTA IV's are
# (whm.py): the layout tables stack into one column, spacer cells and
# pictures go, pictures and Flash shrink to the width with their shape kept,
# and no text is under 13 pixels. The width each picture was drawn at is kept
# first, for its image map.
MOBILE_REFLOW_JS = r"""() => {
  for (const img of document.querySelectorAll('img')) {
    const r = img.getBoundingClientRect();
    img.dataset.w0 = r.width;
  }
  const flash = 'embed, object, ruffle-player, ruffle-object, ruffle-embed';
  for (const el of document.querySelectorAll(flash)) {
    const r = el.getBoundingClientRect();
    if (r.width > 0 && r.height > 0) el.style.setProperty('aspect-ratio', r.width + ' / ' + r.height, 'important');
  }
  const spacer = img => img.naturalWidth <= 2 || img.naturalHeight <= 2 ||
    (/spacer|pixel|blank|clear|trans|shim/i.test(img.src) && img.naturalWidth * img.naturalHeight <= 400);
  for (const img of [...document.querySelectorAll('img')]) if (spacer(img)) img.remove();
  for (const td of [...document.querySelectorAll('td, th')]) {
    if (!td.textContent.trim() && !td.querySelector('img, input, select, textarea, ' + flash)) td.remove();
  }
  const css = document.createElement('style');
  css.textContent = `
    html, body { width: auto !important; min-width: 0 !important; margin: 0 !important; overflow-x: hidden !important; }
    body { padding: 6px !important; box-sizing: border-box; }
    table, tbody, thead, tfoot, tr, td, th { display: block !important; width: auto !important;
      min-width: 0 !important; height: auto !important; box-sizing: border-box; }
    td, th { text-align: left; }
    td[align=center], th[align=center], td[align=middle], center td { text-align: center; }
    div, p, span, font, center, form, blockquote, ul, ol, layer, ilayer { max-width: 100% !important;
      min-width: 0 !important; box-sizing: border-box; }
    div[style*="absolute"], span[style*="absolute"] { position: static !important; }
    div[style*="width"] { width: auto !important; }
    img { max-width: 100% !important; height: auto !important; }
    ${flash} { width: 100% !important; height: auto !important; max-width: 100% !important; }
    * { overflow-wrap: break-word; }
  `;
  document.head.appendChild(css);
  for (const el of document.querySelectorAll('body *')) {
    const size = parseFloat(getComputedStyle(el).fontSize);
    if (size && size < 13 && el.textContent.trim()) el.style.setProperty('font-size', '13px', 'important');
  }
}"""


RUFFLE_CLEANUP_JS = r"""() => {
  for (const el of document.querySelectorAll('ruffle-player, ruffle-object, ruffle-embed')) {
    const root = el.shadowRoot;
    const text = root ? root.textContent : '';
    if (/went wrong|cannot parse|failed/i.test(text)) el.remove();
  }
  for (const el of document.querySelectorAll('#wm-ipp-base, #wm-ipp, #donato')) el.remove();
}"""


def archive_address(where):
    return where.lower().replace('www.', '', 1) if where.lower().startswith('www.') else where.lower()


_fetched = {}


def fetch(url, tries=3):
    """One file from the archive, by Python rather than Chrome (it goes
    through whatever proxy the machine has), kept for the rest of the run.
    None when the archive will not give it."""
    import time, urllib.request, urllib.error
    if url in _fetched:
        return _fetched[url]
    got = None
    for attempt in range(tries):
        try:
            req = urllib.request.Request(url, headers={'User-Agent': 'valkyrie-phone web pack builder'})
            with urllib.request.urlopen(req, timeout=40) as r:
                got = (r.status, r.headers.get('Content-Type', 'application/octet-stream'), r.read())
            break
        except urllib.error.HTTPError as e:
            if e.code in (403, 404, 410):
                break
        except Exception:
            pass
        time.sleep(2 * (attempt + 1))
    _fetched[url] = got
    return got


def render_archive(browser, entries, mobile):
    """The real pages from the Wayback Machine: laid out again for the phone's
    width (MOBILE_REFLOW_JS), or with --desktop each at its own width, fitted
    to ARCHIVE_WIDTH. Pages that will not load are left out (the written ones
    stand in)."""
    def serve(route):
        url = route.request.url
        # Asked for without a mode (Ruffle fetching a Flash file does this)
        # the archive answers with its own viewer page; id_ gives the file.
        if route.request.resource_type != 'document':
            url = re.sub(r'(web\.archive\.org/web/\d+)/', r'\1id_/', url)
        got = fetch(url)
        if got is None:
            route.fulfill(status=404, body=b'')
        else:
            route.fulfill(status=200, headers={'Content-Type': got[1]}, body=got[2])

    link_map = {archive_address(where): key for key, where, _ in entries}
    # Also the plain folder, for a link to "epsilonprogram/".
    for key, where, _ in entries:
        if key.endswith('/index'):
            link_map[archive_address(where).rsplit('/', 1)[0] + '/'] = key
            link_map[archive_address(where).rsplit('/', 1)[0]] = key
    out = []
    for key, where, year in entries:
        url = 'https://web.archive.org/web/%sif_/http://%s' % (year, where)
        # A tab of its own: nothing a failed page left going reaches the next.
        # Loaded at its own width either way: the reflow reads the sizes its
        # pictures were drawn at before narrowing the window.
        tab = browser.new_page(viewport={'width': 1024, 'height': 768},
                               device_scale_factor=MOBILE_PIXEL_RATIO if mobile else 1)
        tab.route('**/*', serve)
        try:
            tab.goto(url, wait_until='load', timeout=300000)
            # Flash, played by the archive's Ruffle, gets a moment to start;
            # one that will not is taken out rather than left as an error.
            tab.wait_for_timeout(9000 if key.split('/')[0] in FLASH_SITES else 3000)
            tab.evaluate(RUFFLE_CLEANUP_JS)
            if mobile:
                tab.evaluate(MOBILE_REFLOW_JS)
                tab.set_viewport_size({'width': MOBILE_WIDTH, 'height': 480})
                tab.wait_for_timeout(1000)
            info = tab.evaluate(ARCHIVE_LINKS_JS, link_map)
            title = tab.title().strip() or key.split('/')[0]
            shot = tab.screenshot(full_page=True)
        except Exception as e:  # the archive is slow or has lost it: the written page stands in
            print('  %s: not from the archive (%s)' % (key, str(e).splitlines()[0]), flush=True)
            continue
        finally:
            tab.close()
        img = Image.open(io.BytesIO(shot)).convert('RGB')
        if mobile:
            # The phone's width already, at its pixels a point.
            left, k = 0, MOBILE_PIXEL_RATIO
        else:
            left, right = info['left'], info['right']
            img = img.crop((left, 0, max(left + 1, min(right, img.width)), img.height))
            k = ARCHIVE_WIDTH / img.width
            img = img.resize((ARCHIVE_WIDTH, max(1, round(img.height * k))), Image.LANCZOS)
        png = io.BytesIO()
        img.quantize(256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE).save(png, 'PNG', optimize=True)
        for link in info['links']:
            link['rects'] = [[(r[0] - left) * k, r[1] * k, r[2] * k, r[3] * k] for r in link['rects']]
        out.append(dict(key=key, title=title, w=img.width, h=img.height, png=png.getvalue(), links=info['links']))
        print('  %s from the archive' % key, flush=True)
    return out


def render(pages, mobile, archive=()):
    from playwright.sync_api import sync_playwright
    out = []
    with sync_playwright() as p:
        browser = p.chromium.launch(channel='chrome')
        width, ratio = (MOBILE_WIDTH, MOBILE_PIXEL_RATIO) if mobile else (640, 1.0)
        tab = browser.new_page(viewport={'width': width, 'height': 480}, device_scale_factor=ratio)
        for i, (key, title, html_path) in enumerate(pages):
            tab.goto(pathlib.Path(html_path).resolve().as_uri())
            info = tab.evaluate(LINKS_JS)
            shot = tab.screenshot(full_page=True)
            img = Image.open(io.BytesIO(shot)).convert('RGB')
            png = io.BytesIO()
            img.quantize(256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE).save(png, 'PNG', optimize=True)
            # Links in the picture's own pixels.
            for link in info['links']:
                link['rects'] = [[v * ratio for v in r] for r in link['rects']]
            out.append(dict(key=key, title=title, w=img.width, h=img.height, png=png.getvalue(), links=info['links']))
            if i % 50 == 0:
                print('%d / %d  %s' % (i, len(pages), key), flush=True)
        if archive:
            print('fetching %d pages from the Wayback Machine...' % len(archive), flush=True)
            got = render_archive(browser, archive, mobile)
            # The real page stands in for the written one of the same name.
            # A site is taken from the archive whole or not at all: only one
            # whose front page came through replaces the written one, so the
            # pages it links to are the real ones as well.
            real_sites = {p['key'].split('/')[0] for p in got if p['key'].endswith('/index')}
            got = [p for p in got if p['key'].split('/')[0] in real_sites]
            out = [p for p in out if p['key'].split('/')[0] not in real_sites] + got
            for site in sorted({p['key'].split('/')[0] for p in (dict(key=k) for k, _, _ in archive)} - real_sites):
                print('  %s: its front page did not come - the written pages stand in' % site, flush=True)
        browser.close()
    return out


def blank(page):
    """Whether a page shows nothing at all: one colour from edge to edge."""
    lo, hi = Image.open(io.BytesIO(page['png'])).convert('L').getextrema()
    return lo == hi


def pack(pages, dest):
    # A front page GTA IV leaves empty for a script to fill opens on the
    # site's first page that has something on it instead.
    for p in pages:
        site, name = p['key'].split('/', 1)
        if name == 'index' and blank(p):
            others = [q for q in pages if q['key'].startswith(site + '/') and q is not p and not blank(q)]
            if others:
                first = min(others, key=lambda q: q['key'])
                print('%s is empty; it opens on %s' % (p['key'], first['key']))
                p.update({k: first[k] for k in ('title', 'w', 'h', 'png', 'links')})
    index = {p['key']: i for i, p in enumerate(pages)}
    strings = bytearray()
    string_at = {}

    def string(s):
        if s not in string_at:
            string_at[s] = len(strings)
            strings.extend(s.encode('cp1252', 'replace') + b'\0')
        return string_at[s]

    links = []
    table = []
    for p in pages:
        first = len(links)
        for link in p['links']:
            href = link['href']
            if href.startswith('ext:'):
                target = -3 - string(href[4:])
            else:
                target = -2 if href == 'back' else index.get(href, -1)
            for x, y, w, h in link['rects']:
                links.append(struct.pack('<iHHHH', target, max(0, int(x)), max(0, int(y)),
                                         max(1, int(round(w))), max(1, int(round(h)))))
        table.append([string(p['key']), string(p['title']), p['w'], p['h'], 0, len(p['png']), first,
                      len(links) - first])

    header = 24
    links_at = header + 28 * len(pages)
    strings_at = links_at + 12 * len(links)
    png_at = strings_at + len(strings)
    for row, p in zip(table, pages):
        row[4] = png_at
        png_at += len(p['png'])

    with open(dest, 'wb') as f:
        f.write(struct.pack('<4sIIIII', b'VWEB', 2, len(pages), len(links), links_at, strings_at))
        for row in table:
            f.write(struct.pack('<IIHHIIII', *row))
        f.writelines(links)
        f.write(strings)
        for p in pages:
            f.write(p['png'])


def read_pack(path):
    """The pages of a pack already built, as pack() takes them."""
    b = open(path, 'rb').read()
    magic, version, count, nlinks, links_at, strings_at = struct.unpack_from('<4sIIIII', b, 0)
    if magic != b'VWEB' or version not in (1, 2):
        raise SystemExit('%s is not a page pack' % path)

    def string(at):
        end = b.index(b'\0', strings_at + at)
        return b[strings_at + at:end].decode('cp1252')

    rows = [struct.unpack_from('<IIHHIIII', b, 24 + 28 * i) for i in range(count)]
    keys = [string(r[0]) for r in rows]
    pages = []
    for key_at, title_at, w, h, png_at, png_size, first, n in rows:
        links = []
        for j in range(first, first + n):
            target, x, y, lw, lh = struct.unpack_from('<iHHHH', b, links_at + 12 * j)
            if target == -1:
                continue
            href = 'back' if target == -2 else 'ext:' + string(-target - 3) if target <= -3 else keys[target]
            links.append({'href': href, 'rects': [(x, y, lw, lh)]})
        pages.append({'key': string(key_at), 'title': string(title_at), 'w': w, 'h': h,
                      'png': b[png_at:png_at + png_size], 'links': links})
    return pages


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('html', nargs='?', help="GTA IV's pc\\html folder (not with --sa-only)")
    ap.add_argument('out', nargs='?', help='where to write valkyrie-web.dat')
    ap.add_argument('--sa-only', action='store_true', help="only San Andreas' own sites, no GTA IV")
    ap.add_argument('--no-archive', action='store_true',
                    help="San Andreas' sites as written here only, nothing fetched from the Wayback Machine")
    ap.add_argument('--with', dest='with_pack',
                    help="take San Andreas' sites from this pack already built, rather than laying them out again")
    ap.add_argument('--gxt', help="GTA IV's pc\\text\\american.gxt, for the lines pages take from it")
    ap.add_argument('--desktop', action='store_true', help="GTA IV's own 640-wide layout instead of the phone's")
    ap.add_argument('--keep', help='keep the converted HTML in this folder')
    args = ap.parse_args()

    if args.sa_only and args.out is None:
        args.html, args.out = None, args.html
    if args.out is None or (args.html is None and not args.sa_only):
        ap.error('give GTA IV\'s html folder and the pack to write, or --sa-only and the pack')
    gxt = Gxt(args.gxt) if args.gxt else None
    work = args.keep or tempfile.mkdtemp(prefix='valkyrie-web-')
    try:
        pages = [] if args.sa_only else convert(args.html, work, gxt, not args.desktop)
        print('converted %d pages' % len(pages))
        if args.with_pack:
            kept = read_pack(args.with_pack)
            print("with %d of San Andreas' own, from %s" % (len(kept), args.with_pack))
            rendered = render(pages, not args.desktop, ()) + kept
        else:
            ours = sa_pages()
            print("with %d of San Andreas' own" % len(ours))
            pages += ours
            rendered = render(pages, not args.desktop, () if args.no_archive else ARCHIVE)
        pack(rendered, args.out)
        print('wrote %s (%.1f MB)' % (args.out, os.path.getsize(args.out) / 1e6))
    finally:
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    main()
