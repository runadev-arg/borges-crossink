// Real, small EPUBs, built byte by byte: the server validates them for real and the
// firmware hashes them for real. All entries are stored, no compression, so the file's
// offsets, and therefore its partial MD5, are easy to reason about.

import crypto from "node:crypto";
import zlib from "node:zlib";

function xml(text) {
  return String(text).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
}

function zip(entries) {
  const locals = [];
  const centrals = [];
  let offset = 0;
  for (const { name, data } of entries) {
    const nameBytes = Buffer.from(name, "utf8");
    const crc = zlib.crc32(data);
    const local = Buffer.alloc(30);
    local.writeUInt32LE(0x04034b50, 0);
    local.writeUInt16LE(20, 4);
    local.writeUInt16LE(0x0800, 6);
    local.writeUInt16LE(0, 8);
    local.writeUInt32LE(0, 10);
    local.writeUInt32LE(crc, 14);
    local.writeUInt32LE(data.length, 18);
    local.writeUInt32LE(data.length, 22);
    local.writeUInt16LE(nameBytes.length, 26);
    locals.push(local, nameBytes, data);

    const central = Buffer.alloc(46);
    central.writeUInt32LE(0x02014b50, 0);
    central.writeUInt16LE(20, 4);
    central.writeUInt16LE(20, 6);
    central.writeUInt16LE(0x0800, 8);
    central.writeUInt16LE(0, 10);
    central.writeUInt32LE(0, 12);
    central.writeUInt32LE(crc, 16);
    central.writeUInt32LE(data.length, 20);
    central.writeUInt32LE(data.length, 24);
    central.writeUInt16LE(nameBytes.length, 28);
    central.writeUInt32LE(offset, 42);
    centrals.push(central, nameBytes);
    offset += local.length + nameBytes.length + data.length;
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

export function buildEpub({ title, author = "Autor de Prueba", paragraphs = 40, padBytes = 0, seed = "" }) {
  const uid = crypto.randomUUID();
  const bodyParts = [];
  for (let i = 0; i < paragraphs; i += 1) {
    bodyParts.push(`<p>Parrafo ${i} de ${xml(title)}. ${xml(seed)} Las palabras siguen y siguen para que haya texto real. ${uid}</p>`);
  }
  const chapter = `<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Capitulo</title></head><body><h1>Capitulo uno</h1>${bodyParts.join("\n")}</body></html>`;
  const opf = `<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="uid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="uid">urn:uuid:${uid}</dc:identifier>
    <dc:title>${xml(title)}</dc:title>
    <dc:creator>${xml(author)}</dc:creator>
    <dc:language>es</dc:language>
  </metadata>
  <manifest>
    <item id="c1" href="ch1.xhtml" media-type="application/xhtml+xml"/>${padBytes ? '\n    <item id="pad" href="pad.bin" media-type="application/octet-stream"/>' : ""}
  </manifest>
  <spine><itemref idref="c1"/></spine>
</package>`;
  const container = `<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>`;
  const entries = [
    { name: "mimetype", data: Buffer.from("application/epub+zip") },
    { name: "META-INF/container.xml", data: Buffer.from(container, "utf8") },
    { name: "OEBPS/content.opf", data: Buffer.from(opf, "utf8") },
    { name: "OEBPS/ch1.xhtml", data: Buffer.from(chapter, "utf8") },
  ];
  if (padBytes) entries.push({ name: "OEBPS/pad.bin", data: crypto.randomBytes(padBytes) });
  return zip(entries);
}

// KOReader's util.partialMD5, written independently of the core: 1 KiB at
// lshift(1024, 2i) for i = -1..10, with LuaJIT's shift wrapping i = -1 to offset 0.
export function koreaderPartialMd5(bytes) {
  const hash = crypto.createHash("md5");
  for (let i = -1; i <= 10; i += 1) {
    const offset = i < 0 ? 0 : 1024 * 4 ** i;
    if (offset >= bytes.length) break;
    hash.update(bytes.subarray(offset, offset + 1024));
  }
  return hash.digest("hex");
}

export function sha256(bytes) {
  return crypto.createHash("sha256").update(bytes).digest("hex");
}
