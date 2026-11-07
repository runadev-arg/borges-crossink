// Real multi-chapter EPUBs, stored and not compressed, each chapter with its own numbered
// paragraphs: a screenshot says which chapter it is on, and an xpointer KOReader would
// write can be computed from the structure, since this file is what decides the structure.

import crypto from "node:crypto";
import zlib from "node:zlib";

const xml = (text) => String(text).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");

// deflate: every entry but mimetype compressed, as a real EPUB is. Then reading a chapter takes
// inflate and its 32 KB window, which a stored book never asks for.
function zip(entries, { deflate = false } = {}) {
  const locals = [];
  const centrals = [];
  let offset = 0;
  for (const { name, data } of entries) {
    const nameBytes = Buffer.from(name, "utf8");
    const crc = zlib.crc32(data);
    const packed = deflate && name !== "mimetype";
    const body = packed ? zlib.deflateRawSync(data) : data;
    const local = Buffer.alloc(30);
    local.writeUInt32LE(0x04034b50, 0);
    local.writeUInt16LE(20, 4);
    local.writeUInt16LE(0x0800, 6);
    local.writeUInt16LE(packed ? 8 : 0, 8);
    local.writeUInt32LE(crc, 14);
    local.writeUInt32LE(body.length, 18);
    local.writeUInt32LE(data.length, 22);
    local.writeUInt16LE(nameBytes.length, 26);
    locals.push(local, nameBytes, body);
    const central = Buffer.alloc(46);
    central.writeUInt32LE(0x02014b50, 0);
    central.writeUInt16LE(20, 4);
    central.writeUInt16LE(20, 6);
    central.writeUInt16LE(0x0800, 8);
    central.writeUInt16LE(packed ? 8 : 0, 10);
    central.writeUInt32LE(crc, 16);
    central.writeUInt32LE(body.length, 20);
    central.writeUInt32LE(data.length, 24);
    central.writeUInt16LE(nameBytes.length, 28);
    central.writeUInt32LE(offset, 42);
    centrals.push(central, nameBytes);
    offset += local.length + nameBytes.length + body.length;
  }
  const centralBytes = Buffer.concat(centrals);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0);
  end.writeUInt16LE(entries.length, 8);
  end.writeUInt16LE(entries.length, 10);
  end.writeUInt32LE(centralBytes.length, 12);
  end.writeUInt32LE(offset, 16);
  return Buffer.concat([...locals, centralBytes, end]);
}

const FILLER = "Las palabras siguen y siguen por el corredor de la biblioteca, hexagono tras hexagono, para que cada pagina tenga texto de verdad y el lector tenga que pasarla entera.";

// chapters are the same length, so spine index i of n sits at about i/n of the book.
export function buildBook({ title, author = "Autor de Prueba", chapters = 10, paragraphs = 40, padBytes = 0, deflate = false }) {
  const uid = crypto.randomUUID();
  const entries = [{ name: "mimetype", data: Buffer.from("application/epub+zip") }];
  entries.push({
    name: "META-INF/container.xml",
    data: Buffer.from(`<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>`),
  });
  const ids = Array.from({ length: chapters }, (_, i) => i + 1);
  const manifest = ids.map((n) => `<item id="c${n}" href="ch${n}.xhtml" media-type="application/xhtml+xml"/>`).join("\n    ");
  const spine = ids.map((n) => `<itemref idref="c${n}"/>`).join("");
  entries.push({
    name: "OEBPS/content.opf",
    data: Buffer.from(`<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="uid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="uid">urn:uuid:${uid}</dc:identifier>
    <dc:title>${xml(title)}</dc:title>
    <dc:creator>${xml(author)}</dc:creator>
    <dc:language>es</dc:language>
  </metadata>
  <manifest>
    <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
    ${manifest}${padBytes ? '\n    <item id="pad" href="pad.bin" media-type="application/octet-stream"/>' : ""}
  </manifest>
  <spine toc="ncx">${spine}</spine>
</package>`),
  });
  const navPoints = ids
    .map((n) => `<navPoint id="n${n}" playOrder="${n}"><navLabel><text>Capitulo ${n}</text></navLabel><content src="ch${n}.xhtml"/></navPoint>`)
    .join("\n    ");
  entries.push({
    name: "OEBPS/toc.ncx",
    data: Buffer.from(`<?xml version="1.0" encoding="UTF-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
  <head><meta name="dtb:uid" content="urn:uuid:${uid}"/></head>
  <docTitle><text>${xml(title)}</text></docTitle>
  <navMap>
    ${navPoints}
  </navMap>
</ncx>`),
  });
  for (const n of ids) {
    const body = [];
    for (let k = 1; k <= paragraphs; k += 1) body.push(`<p>Capitulo ${n}, parrafo ${k}. ${FILLER}</p>`);
    entries.push({
      name: `OEBPS/ch${n}.xhtml`,
      data: Buffer.from(`<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Capitulo ${n}</title></head><body><h1>Capitulo ${n}</h1>
${body.join("\n")}
</body></html>`),
    });
  }
  if (padBytes) entries.push({ name: "OEBPS/pad.bin", data: crypto.randomBytes(padBytes) });
  return zip(entries, { deflate });
}

// DocFragment is 1-based over the spine, as crengine writes it. The h1 is a sibling of the
// p elements, so p[k] is the kth paragraph.
export const xpointerOf = (chapter, paragraph = 1, offset = 0) => `/body/DocFragment[${chapter}]/body/p[${paragraph}]/text().${offset}`;

// progress.bin: spine, page, page count, then an optional visible text offset, little endian.
export function decodeProgress(bytes) {
  const out = { spine: bytes.readUInt16LE(0), page: bytes.readUInt16LE(2) };
  if (bytes.length >= 6) out.pageCount = bytes.readUInt16LE(4);
  if (bytes.length === 10) out.visibleTextOffset = bytes.readUInt32LE(6);
  return out;
}
