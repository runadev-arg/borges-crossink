// Raw BGRA from the SDL proxy to an 8-bit grayscale PNG. No dependencies, and no BMP left
// behind: the panel is gray anyway.

import fs from "node:fs";
import zlib from "node:zlib";

function chunk(type, data) {
  const head = Buffer.alloc(4);
  head.writeUInt32BE(data.length, 0);
  const body = Buffer.concat([Buffer.from(type, "ascii"), data]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(zlib.crc32(body) >>> 0, 0);
  return Buffer.concat([head, body, crc]);
}

export function rawToPng(rawPath, width, height, pngPath) {
  const raw = fs.readFileSync(rawPath);
  const rows = Buffer.alloc((width + 1) * height);
  let out = 0;
  for (let y = 0; y < height; y += 1) {
    rows[out++] = 0;
    for (let x = 0; x < width; x += 1) {
      rows[out++] = raw[(y * width + x) * 4 + 1];
    }
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8;
  ihdr[9] = 0;
  const png = Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), chunk("IHDR", ihdr), chunk("IDAT", zlib.deflateSync(rows)), chunk("IEND", Buffer.alloc(0))]);
  fs.writeFileSync(pngPath, png);
  fs.rmSync(rawPath, { force: true });
}
