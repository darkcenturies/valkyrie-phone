"""Read GTA IV's .whm web pages and write them out as plain HTML and PNGs.

A .whm is a RAGE RSC05 resource: "RSC\\x05", a type, a flags word giving the
two segment sizes, then one zlib stream holding the system segment (the page
already parsed into a DOM, every node carrying its computed style) followed
by the graphics segment (the page's textures). Pointers into the system
segment are 0x5xxxxxxx, into the graphics segment 0x6xxxxxxx.

No description of the format has been published; these offsets were worked
out from the 628 pages the game ships.

The document: +0x00 points at the root <html> node.

Every node (0xF0 bytes; tables 0x110):
  +0x00 class: 0x6A0880 element, 0x6A088C text, 0x6A0944 table,
        0x6A09D4 table row or cell
  +0x08 parent, +0x0C children (array of pointers), +0x10 u16 child count
  +0x14 display: 0x0F block, 0x10 table row, 0x11 table cell, 0x12 inline
  +0x18 width, +0x1C height (floats, -1 for auto)
  +0x20 / +0x24 an image's own width and height
  +0x34 background colour (ARGB), used when the byte at +0xCC is set
  +0x38 background image (pointer to a texture)
  +0x48 text colour
  +0x4C text-align: 0 left, 1 right, 2 center, 3 justify, -1 inherited
  +0x50 vertical-align: 2 baseline, 4 top, 5 bottom, 6 middle
  +0x54 text-decoration: 0x13 none, 0x15 underline
  +0x5C font size in pixels
  +0x6C four borders (top, right, bottom, left) of {colour, style 0x13 none /
        0x14 solid, float width}
  +0x9C margins: top, right, left, bottom
  +0xBC cell padding, +0xC0 cell spacing
  +0xC4 colspan, +0xC8 rowspan
  +0xD0 link colour
Text nodes: +0xD8 the text. Elements: +0xD8 the tag (TAGS below), +0xE0 its
one attribute - an <a>'s href, an <img>'s src, or the name of a slot the
game fills in (tags 0x26 from the text file, 0x27 from a script).

Textures (0x50 bytes, class 0x6B1D94): +0x14 name, +0x1C u16 width,
+0x1E u16 height, +0x20 the D3D format (DXT1, DXT3, DXT5 or 21 for
A8R8G8B8), +0x48 the pixels in the graphics segment.
"""
import html, io, os, re, struct, zlib
from PIL import Image

VT_ELEMENT, VT_TEXT, VT_TABLE, VT_ROWCELL = 0x6a0880, 0x6a088c, 0x6a0944, 0x6a09d4
TEXTURE_VT = 0x6b1d94

# Element tag ids (node +0xD8). Names for ids seen only inside text are the
# best match for how the pages use them.
TAGS = {0: 'html', 1: 'title', 2: 'a', 3: 'body', 5: 'br', 6: 'hr', 0xb: 'div',
        0xd: 'i', 0xe: 'head', 0x15: 'img', 0x18: 'li', 0x19: 'meta', 0x1b: 'ul',
        0x1c: 'p', 0x1e: 'span', 0x1f: 'b', 0x20: 'style', 0x21: 'table',
        0x22: 'tr', 0x23: 'th', 0x24: 'td', 0x26: 'span', 0x27: 'span', 0x28: 'strong'}
# The layout for a phone's narrow screen, the way the small-screen browsers
# of 2007 reflowed desktop pages: layout tables become rows that wrap, each
# cell keeping its width only as a preference; fixed sizes go; pictures
# shrink to fit; cells that only held space disappear.
MOBILE_MIN_FONT = 13
MOBILE_CSS = (
    'html,body{width:auto!important;max-width:100%;overflow-x:hidden}'
    'body{margin:0!important}'
    'table,tbody,thead,tfoot{display:block!important;width:auto!important;height:auto!important;'
    'max-width:100%;margin-left:auto;margin-right:auto}'
    'tr{display:flex!important;flex-wrap:wrap;width:auto!important;height:auto!important}'
    'td,th{display:block!important;flex:1 1 var(--w,auto);width:auto!important;height:auto!important;'
    'min-width:0;max-width:100%;min-height:var(--h,0);box-sizing:border-box}'
    'td.spacer,th.spacer,td.aside,th.aside{display:none!important}'
    'td.slice,th.slice{flex:0 0 var(--pct)!important;font-size:0!important;line-height:0}'
    'td.txt,th.txt{padding-left:5px!important;padding-right:5px!important}'
    'table.grid{display:table!important;width:100%!important}'
    'table.grid>tbody{display:table-row-group!important}'
    'table.grid>tbody>tr{display:table-row!important}'
    'table.grid>tbody>tr>td,table.grid>tbody>tr>th{display:table-cell!important}'
    'p,div,ul,li{width:auto!important;height:auto!important;max-width:100%}'
    'img{max-width:100%!important;height:auto!important}'
    'span{max-width:100%}'
    '*{overflow-wrap:anywhere}'
)
ALIGN = {0: 'left', 1: 'right', 2: 'center', 3: 'justify'}
VALIGN = {2: 'baseline', 4: 'top', 5: 'bottom', 6: 'middle'}


class Page:
    def __init__(self, path):
        raw = open(path, 'rb').read()
        flags = struct.unpack_from('<I', raw, 8)[0]
        self.sys_size = (flags & 0x7FF) << (((flags >> 11) & 0xF) + 8)
        self.d = zlib.decompressobj().decompress(raw[12:])
        self.textures = {}
        self._scan_textures()

    def u(self, o): return struct.unpack_from('<I', self.d, o)[0]
    def f(self, o): return struct.unpack_from('<f', self.d, o)[0]

    def ptr(self, v):
        return (v & 0xFFFFFFF) if v >> 28 == 5 else None

    def cstr(self, o):
        return self.d[o:self.d.index(b'\0', o)].decode('cp1252', 'replace')

    def _scan_textures(self):
        d = self.d
        for o in range(0, self.sys_size - 0x50, 4):
            if self.u(o) != TEXTURE_VT or self.u(o + 0x14) >> 28 != 5:
                continue
            name = self.cstr(self.ptr(self.u(o + 0x14)))
            w, h = struct.unpack_from('<HH', d, o + 0x1C)
            fmt = d[o + 0x20:o + 0x24]
            stride = struct.unpack_from('<H', d, o + 0x24)[0]
            data = self.sys_size + (self.u(o + 0x48) & 0xFFFFFFF)
            self.textures[o] = dict(name=name, w=w, h=h, fmt=fmt, stride=stride, data=data)

    def texture_image(self, t):
        w, h, fmt = t['w'], t['h'], t['fmt']
        bw, bh = (w + 3) // 4, (h + 3) // 4
        if fmt in (b'DXT1', b'DXT3', b'DXT5'):
            size = bw * bh * (8 if fmt == b'DXT1' else 16)
            dds = dds_header(bw * 4, bh * 4, fmt) + self.d[t['data']:t['data'] + size]
            img = Image.open(io.BytesIO(dds)).convert('RGBA').crop((0, 0, w, h))
        else:  # A8R8G8B8
            img = Image.frombuffer('RGBA', (w, h), self.d[t['data']:t['data'] + w * h * 4],
                                   'raw', 'BGRA', 0, 1)
        return img

    def root(self):
        return self.ptr(self.u(0))

    def children(self, o):
        ch = self.ptr(self.u(o + 0xC))
        n = self.u(o + 0x10) & 0xFFFF
        return [self.ptr(self.u(ch + 4 * i)) for i in range(n)] if ch else []


def dds_header(w, h, fmt):
    hdr = struct.pack('<4sIIIIIII', b'DDS ', 124, 0x1007, h, w, 0, 0, 1)
    hdr += b'\0' * 44
    hdr += struct.pack('<II4sIIIII', 32, 4, fmt, 0, 0, 0, 0, 0)
    hdr += struct.pack('<IIIII', 0x1000, 0, 0, 0, 0)
    return hdr


def argb(v):
    a, r, g, b = v >> 24, (v >> 16) & 255, (v >> 8) & 255, v & 255
    return '#%02x%02x%02x' % (r, g, b) if a == 255 else 'rgba(%d,%d,%d,%.3f)' % (r, g, b, a / 255)


def texture_key(src):
    s = src.replace('\\', '/').lower()
    s = re.sub(r'\.(jpg|jpeg|gif|png|bmp|dds|tga)$', '', s)
    return s


class Converter:
    def __init__(self, page, site, name, img_dir, img_url, gxt=None, mobile=False):
        self.p = page
        self.site = site
        self.name = name
        self.img_dir = img_dir
        self.img_url = img_url
        self.saved = {}
        self.by_name = {}
        for o, t in page.textures.items():
            self.by_name.setdefault(texture_key(t['name']), o)
        self.title = ''
        self.links = []
        self.missing = set()
        self.gxt = gxt or {}
        self.mobile = mobile
        # The width of the picture-only cell being written, if any: the
        # pictures in it are sized as shares of it.
        self.slice_w = None

    def image(self, tex_off):
        if tex_off in self.saved:
            return self.saved[tex_off]
        t = self.p.textures[tex_off]
        fn = re.sub(r'[^a-z0-9_.-]+', '_', t['name'].lower()) + '.png'
        path = os.path.join(self.img_dir, fn)
        if not os.path.exists(path):
            os.makedirs(self.img_dir, exist_ok=True)
            self.p.texture_image(t).save(path, optimize=True)
        self.saved[tex_off] = self.img_url + fn
        return self.saved[tex_off]

    def find_texture(self, src):
        k = texture_key(src)
        if k in self.by_name:
            return self.by_name[k]
        base = k.rsplit('/', 1)[-1]
        for name, o in self.by_name.items():
            if name.rsplit('/', 1)[-1] == base:
                return o
        return None

    def hoisted(self, o):
        """The texture of a big picture that words were laid over, for the
        narrow screen to show above them instead, or None."""
        if not self.mobile:
            return None
        p = self.p
        bgtex = p.ptr(p.u(o + 0x38))
        tex = p.textures.get(bgtex)
        if not tex or tex['w'] < 200 or tex['h'] < 120:
            return None
        w = p.f(o + 0x18)
        if w > 0 and tex['w'] < 0.6 * w:
            return None
        return bgtex if self.content(o)[0] else None

    def hoist_html(self, o):
        """The picture, shown first, at the width of its box."""
        t = self.hoisted(o)
        if t is None:
            return ''
        return '<img src="%s" style="display:block;width:100%%!important;height:auto">' % self.image(t)

    def ground(self, tex_off):
        """The colour along the bottom of a picture: what the words laid
        over it were written on."""
        img = self.p.texture_image(self.p.textures[tex_off]).convert('RGB')
        strip = img.crop((0, int(img.height * 0.9), img.width, img.height)).resize((1, 1), Image.BOX)
        r, g, b = strip.getpixel((0, 0))
        return '#%02x%02x%02x' % (r, g, b)

    def style(self, o, kind):
        p = self.p
        s = []
        w, h = p.f(o + 0x18), p.f(o + 0x1C)
        bgtex = p.ptr(p.u(o + 0x38))
        has_bg_image = bgtex is not None and bgtex in p.textures
        if has_bg_image and self.hoisted(o) is not None:
            # Shown above the words instead (hoist_html); the box keeps the
            # picture's ground colour.
            has_bg_image = False
            if not (p.u(o + 0xCC) & 0xFF):
                s.append('background-color:' + self.ground(bgtex))
        if kind not in ('text',):
            if w >= 0 and kind != 'img':
                s.append('width:%gpx' % w)
                # The mobile layout keeps the width as a preference only.
                s.append('--w:%gpx' % w)
            if h >= 0 and kind != 'img':
                s.append('height:%gpx' % h)
        has_bg = p.u(o + 0xCC) & 0xFF
        bg = p.u(o + 0x34)
        if has_bg and kind not in ('text',):
            s.append('background-color:' + argb(bg))
        if has_bg_image:
            s.append("background-image:url('%s')" % self.image(bgtex))
            tex = p.textures[bgtex]
            if self.mobile and w >= 200 and tex['w'] >= 0.6 * w:
                # A whole picture rather than a tile: on the narrow screen it
                # shrinks with its box, which keeps the picture's shape.
                s.append('background-size:100% auto;background-repeat:no-repeat')
                # Its shape only holds a box that is nothing but the picture;
                # one with words in grows with them.
                if h > 0 and not self.content(o)[0]:
                    s.append('aspect-ratio:%g/%g' % (w, h))
            elif self.mobile:
                # On the narrow screen a box grows taller than the page drew
                # it, and a picture that filled it would repeat down it. It
                # repeats only the ways it was a tile to begin with.
                tile_x = tex['w'] < (0.5 * w if w > 0 else 100)
                tile_y = tex['h'] < (0.5 * h if h > 0 else 100)
                s.append('background-repeat:' + ('repeat' if tile_x and tile_y else 'repeat-x' if tile_x
                                                 else 'repeat-y' if tile_y else 'no-repeat'))
        s.append('color:' + argb(p.u(o + 0x48)))
        al = p.u(o + 0x4C)
        if al in ALIGN and kind != 'text':
            s.append('text-align:' + ALIGN[al])
        va = p.u(o + 0x50)
        if va in VALIGN and kind in ('cell', 'row'):
            s.append('vertical-align:' + VALIGN[va])
        s.append('text-decoration:' + ('underline' if p.u(o + 0x54) == 0x15 else 'none'))
        size = p.u(o + 0x5C)
        # On the phone nothing is smaller than the phone's own small print.
        s.append('font-size:%dpx' % (max(size, MOBILE_MIN_FONT) if self.mobile else size))
        if kind in ('table', 'cell', 'row', 'block'):
            sides = ('top', 'right', 'bottom', 'left')
            for i, side in enumerate(sides):
                col, sty, bw = p.u(o + 0x6C + 12 * i), p.u(o + 0x70 + 12 * i), p.f(o + 0x74 + 12 * i)
                if sty == 0x14 and bw > 0:
                    s.append('border-%s:%gpx solid %s' % (side, bw, argb(col)))
        mt, mr, ml, mb = p.f(o + 0x9C), p.f(o + 0xA0), p.f(o + 0xA4), p.f(o + 0xA8)
        if kind in ('block', 'body'):
            s.append('margin:%gpx %gpx %gpx %gpx' % (mt, mr, mb, ml))
        if kind == 'cell':
            s.append('padding:%gpx' % p.f(o + 0xBC))
        return ';'.join(s)

    def content(self, o):
        """What a node shows: (has text, has a link, number of images)."""
        p = self.p
        vt = p.u(o)
        if vt == VT_TEXT:
            return bool(p.cstr(p.ptr(p.u(o + 0xD8))).strip()), False, 0
        text, link, images = False, False, 0
        if vt == VT_ELEMENT:
            tid = p.u(o + 0xD8)
            link = tid == 2
            images = 1 if tid == 0x15 else 0
            text = tid in (0x26, 0x27)
        for c in p.children(o):
            t, l, n = self.content(c)
            text, link, images = text or t, link or l, images + n
        return text, link, images

    def over_picture(self, o):
        """Whether a box is painted with a whole picture (not a tile) that what
        is in it is laid over."""
        tex = self.p.textures.get(self.p.ptr(self.p.u(o + 0x38)))
        w = self.p.f(o + 0x18)
        return bool(tex) and w >= 200 and tex['w'] >= 0.6 * w

    def has_spacer(self, o):
        """Whether a node holds a spacer: a tiny texture stretched to hold a
        place - what pages line things up over a picture with."""
        p = self.p
        if p.u(o) == VT_ELEMENT and p.u(o + 0xD8) == 0x15:
            attr = p.ptr(p.u(o + 0xE0))
            t = self.find_texture(p.cstr(attr)) if attr else None
            tex = p.textures.get(t)
            return bool(tex) and tex['w'] * tex['h'] <= 64
        return any(self.has_spacer(c) for c in p.children(o))

    def rows(self, table):
        return [r for r in self.p.children(table)
                if self.p.u(r) == VT_ROWCELL and TAGS.get(self.p.u(r + 0xD8)) == 'tr']

    def is_grid(self, table):
        """A table of data - a forum's topics, a calendar - rather than one
        laying the page out: several rows of the same number of cells, most
        of them with words in."""
        rows = self.rows(table)
        if len(rows) < 3:
            return False
        counts = [len(self.p.children(r)) for r in rows]
        common = max(set(counts), key=counts.count)
        worded = sum(1 for r in rows if self.content(r)[0])
        return common >= 2 and counts.count(common) >= 0.6 * len(rows) and worded >= 0.5 * len(rows)

    def is_aside(self, cell, row):
        """A narrow column of pictures beside the page's main one: decoration
        the desktop layout had room for."""
        p = self.p
        widths = [max(p.f(c + 0x18), 0.0) for c in p.children(row)]
        total = sum(widths)
        w = max(p.f(cell + 0x18), 0.0)
        if total <= 0 or w <= 0 or w > 0.25 * total or max(widths) < 0.45 * total:
            return False
        text, link, images = self.content(cell)
        return not text and not link and images > 0

    def link_target(self, href):
        """'site/page' for a link; 'back' for the browser's back; None for
        links the game's scripts act on (numbers, '#')."""
        h = href.replace('\\', '/').strip()
        h = re.sub(r'^https?://', '', h, flags=re.I).split('#')[0].strip('/')
        if not h or h.isdigit():
            return None
        if h.lower() == 'back':
            return 'back'
        parts = [x for x in h.split('/') if x]
        # The last part that names a site, and what follows it.
        site_at = max((i for i, x in enumerate(parts) if '.' in x and not re.search(r'\.html?$', x, re.I)),
                      default=None)
        if site_at is None:
            site, page = self.site, parts[-1]
        else:
            site = parts[site_at]
            page = parts[site_at + 1] if site_at + 1 < len(parts) else 'index'
        page = re.sub(r'\.html?$', '', page, flags=re.I) or 'index'
        return site.lower() + '/' + page.lower()

    def node(self, o):
        p = self.p
        vt = p.u(o)
        if vt == VT_TEXT:
            text = p.cstr(p.ptr(p.u(o + 0xD8)))
            return '<span style="%s">%s</span>' % (self.style(o, 'text'), html.escape(text))
        kids = lambda: ''.join(self.node(c) for c in p.children(o))
        if vt == VT_TABLE:
            pad, spc = p.f(o + 0xBC), p.f(o + 0xC0)
            al = p.u(o + 0x4C)
            attrs = ' cellpadding="%g" cellspacing="%g"' % (pad, spc)
            if al == 2:
                attrs += ' align="center"'
            elif al == 1:
                attrs += ' align="right"'
            cls = ' class="grid"' if self.mobile and self.is_grid(o) else ''
            return '%s<table%s%s style="%s">%s</table>' % (self.hoist_html(o), cls, attrs, self.style(o, 'table'),
                                                          kids())
        if vt == VT_ROWCELL:
            tag = TAGS.get(p.u(o + 0xD8), 'td')
            if tag == 'tr':
                return '<tr style="%s">%s</tr>' % (self.style(o, 'row'), kids())
            span = ''
            if p.u(o + 0xC4) > 1:
                span += ' colspan="%d"' % p.u(o + 0xC4)
            if p.u(o + 0xC8) > 1:
                span += ' rowspan="%d"' % p.u(o + 0xC8)
            text, link, images = self.content(o)
            bg_image = p.ptr(p.u(o + 0x38)) is not None
            style = self.style(o, 'cell')
            cls = ''
            # A row made only of pictures - a header or a menu cut into
            # slices - shrinks as a whole on the narrow screen, every cell and
            # picture keeping its share of the width, instead of wrapping.
            row = p.ptr(p.u(o + 0x8))
            table = p.ptr(p.u(row + 0x8)) if row else None
            tw = p.f(table + 0x18) if table else -1.0
            cw = p.f(o + 0x18)
            if cw <= 0 and row and len(p.children(row)) == 1:
                cw = tw  # the only cell: the table's width
            slice_row = (self.mobile and row and tw > 0 and cw > 0 and
                         not any(self.content(c)[0] for c in p.children(row)) and
                         (self.over_picture(table) or self.over_picture(row) or
                          any(self.over_picture(c) or self.has_spacer(c) for c in p.children(row))))
            outer_slice = self.slice_w
            if slice_row:
                self.slice_w = cw
                style += ';--pct:%.4f%%' % min(100.0, cw / tw * 100.0)
            inner = kids()
            self.slice_w = outer_slice
            if slice_row and (images or link):
                cls = ' class="slice"'
            elif text and self.mobile:
                cls = ' class="txt"'
            if not text and not link and not images:
                if bg_image and 0 < p.f(o + 0x1C) <= 300:
                    # A picture drawn as the cell's background: it keeps
                    # its height, or there is nothing to show it in.
                    style += ';--h:%gpx' % p.f(o + 0x1C)
                elif not bg_image:
                    # Nothing in it: spacing for the desktop layout.
                    cls = ' class="spacer"'
            elif self.mobile and self.is_aside(o, p.ptr(p.u(o + 0x8))):
                cls = ' class="aside"'
            return '<%s%s%s style="%s">%s%s</%s>' % (tag, cls, span, style, self.hoist_html(o), inner, tag)
        tid = p.u(o + 0xD8)
        tag = TAGS.get(tid, 'span')
        attr_p = p.ptr(p.u(o + 0xE0))
        attr = p.cstr(attr_p) if attr_p else ''
        if tag == 'html':
            return kids()
        if tag == 'head':
            for c in p.children(o):
                if p.u(c) == VT_ELEMENT and p.u(c + 0xD8) == 1:
                    self.title = ''.join(p.cstr(p.ptr(p.u(t + 0xD8))) for t in p.children(c) if p.u(t) == VT_TEXT).strip()
            return ''
        if tag == 'body':
            return '<body style="%s">%s%s</body>' % (self.style(o, 'body'), self.hoist_html(o), kids())
        if tag == 'br':
            return '<br>'
        if tag == 'img':
            t = self.find_texture(attr)
            w, h = p.f(o + 0x18), p.f(o + 0x1C)
            if w < 0: w = p.f(o + 0x20)
            if h < 0: h = p.f(o + 0x24)
            size = 'width:%gpx;height:%gpx' % (max(w, 0), max(h, 0))
            if self.slice_w and w > 0:
                size = 'width:%.4f%%;height:auto' % min(100.0, w / self.slice_w * 100.0)
            if w > 0 and h > 0:
                # Shrunk to fit a narrow screen, a picture keeps the shape the
                # page gave it, not the texture's own: pages stretch a
                # one-pixel texture into their spacers.
                size += ';aspect-ratio:%g/%g' % (w, h)
            if t is None:
                self.missing.add(attr)
                return '<span style="display:inline-block;vertical-align:bottom;%s"></span>' % size
            return '<img src="%s" style="vertical-align:bottom;%s">' % (self.image(t), size)
        if tag == 'a':
            target = self.link_target(attr) if attr else None
            self.links.append(attr)
            href = target or ''
            return '<a data-href="%s" style="%s">%s</a>' % (html.escape(href), self.style(o, 'inline'), kids())
        if tag in ('p', 'div', 'ul', 'li'):
            return '<%s style="%s">%s%s</%s>' % (tag, self.style(o, 'block'), self.hoist_html(o), kids(), tag)
        if tag == 'hr':
            return '<hr>'
        if tag in ('style', 'meta', 'title'):
            return ''
        if tid == 0x26:
            # A line from the game's text file, when one was given.
            text = self.gxt.get(attr.upper(), '')
            return '<span style="%s">%s</span>' % (self.style(o, 'inline'), html.escape(text).replace('~n~', '<br>'))
        return '<%s style="%s">%s</%s>' % (tag, self.style(o, 'inline'), kids(), tag)

    def convert(self):
        body = self.node(self.p.root())
        css = ('html,body{margin:0;padding:0}body{font-family:Arial,Helvetica,sans-serif;line-height:1.1}'
               'table{border-collapse:separate}img{display:inline-block}p{margin:0}')
        if self.mobile:
            css += MOBILE_CSS
        return ('<!DOCTYPE html><html><head><meta charset="utf-8"><title>%s</title><style>%s</style>'
                '</head>%s</html>' % (html.escape(self.title), css, body))
