/* Cinema movie + subtitle formats. This module is intentionally independent
 * of the browser UI so Node tests can check its output against the C/Python
 * readers used by the calculator. See ../CIN2_FORMAT.md. */

export const WIDTH = 160;
export const HEIGHT = 96;
export const FRAME_BYTES = WIDTH * HEIGHT / 2;
export const HEADER_BYTES = 512;
const COVER_W = 20;
const COVER_H = 12;
const LINE_MAX = 30;

export function crc32(bytes, seed = 0) {
  let value = (seed ^ 0xffffffff) >>> 0;
  for (const byte of bytes) {
    value ^= byte;
    for (let bit = 0; bit < 8; bit++) {
      value = (value >>> 1) ^ ((value & 1) ? 0xedb88320 : 0);
    }
  }
  return (value ^ 0xffffffff) >>> 0;
}

export function color1555(r, g, b) {
  return ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
}

const red5 = c => (c >>> 10) & 31;
const green5 = c => (c >>> 5) & 31;
const blue5 = c => c & 31;

/* A weighted median-cut palette from frames sampled throughout the movie.
 * Reserve index zero for black so letterbox bars stay truly black. */
export function createPalette(samples) {
  if (!samples.length) throw new Error('Could not read any video frames.');
  const counts = new Uint32Array(32768);
  for (const pixels of samples) {
    for (let i = 0; i < pixels.length; i += 4) {
      const index = ((pixels[i] >>> 3) << 10)
        | ((pixels[i + 1] >>> 3) << 5) | (pixels[i + 2] >>> 3);
      counts[index]++;
    }
  }
  const bins = [];
  for (let i = 0; i < counts.length; i++) if (counts[i]) bins.push(i);
  const boxes = [bins];

  const measure = bin => {
    let minR = 31, maxR = 0, minG = 31, maxG = 0, minB = 31, maxB = 0, total = 0;
    for (const i of bin) {
      const r = red5(i), g = green5(i), b = blue5(i);
      if (r < minR) minR = r;
      if (r > maxR) maxR = r;
      if (g < minG) minG = g;
      if (g > maxG) maxG = g;
      if (b < minB) minB = b;
      if (b > maxB) maxB = b;
      total += counts[i];
    }
    const ranges = [maxR - minR, maxG - minG, maxB - minB];
    /* Stronger weight on green matches the CE's perceived brightness. */
    const weighted = [ranges[0] * 2, ranges[1] * 3, ranges[2]];
    const axis = weighted.indexOf(Math.max(...weighted));
    return { axis, score: Math.max(...weighted) * total, total };
  };

  while (boxes.length < 15) {
    let best = -1, bestScore = -1, bestAxis = 0;
    for (let i = 0; i < boxes.length; i++) {
      if (boxes[i].length < 2) continue;
      const m = measure(boxes[i]);
      if (m.score > bestScore) { best = i; bestScore = m.score; bestAxis = m.axis; }
    }
    if (best < 0) break;
    const sorted = boxes[best].slice().sort((a, b) => {
      const av = bestAxis === 0 ? red5(a) : bestAxis === 1 ? green5(a) : blue5(a);
      const bv = bestAxis === 0 ? red5(b) : bestAxis === 1 ? green5(b) : blue5(b);
      return av - bv || a - b;
    });
    let total = 0;
    for (const i of sorted) total += counts[i];
    let prefix = 0, split = 1;
    for (; split < sorted.length; split++) {
      prefix += counts[sorted[split - 1]];
      if (prefix >= total / 2) break;
    }
    boxes.splice(best, 1, sorted.slice(0, split), sorted.slice(split));
  }

  const palette = [0];
  for (const box of boxes) {
    let r = 0, g = 0, b = 0, n = 0;
    for (const i of box) {
      const weight = counts[i];
      r += red5(i) * weight;
      g += green5(i) * weight;
      b += blue5(i) * weight;
      n += weight;
    }
    palette.push(((Math.round(r / n) & 31) << 10)
      | ((Math.round(g / n) & 31) << 5) | (Math.round(b / n) & 31));
  }
  while (palette.length < 16) palette.push(palette[palette.length - 1]);
  return palette;
}

/* Lookup maps each 5-5-5 source color to the closest palette entry. The
 * 32,768-entry table is created once, avoiding sixteen comparisons/pixel. */
export function createColorLookup(palette) {
  if (palette.length !== 16) throw new Error('Movie palette must have 16 colors.');
  const lookup = new Uint8Array(32768);
  for (let c = 0; c < lookup.length; c++) {
    const r = red5(c), g = green5(c), b = blue5(c);
    let best = 0, bestError = Infinity;
    for (let p = 0; p < 16; p++) {
      const dr = r - red5(palette[p]);
      const dg = g - green5(palette[p]);
      const db = b - blue5(palette[p]);
      const error = 2 * dr * dr + 3 * dg * dg + db * db;
      if (error < bestError) { bestError = error; best = p; }
    }
    lookup[c] = best;
  }
  return lookup;
}

const BAYER4 = [0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5];

export function quantizeIndices(rgba, width, height, lookup, dither = false) {
  if (rgba.length !== width * height * 4) throw new Error('Invalid picture dimensions.');
  const indices = new Uint8Array(width * height);
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const pixel = y * width + x, offset = pixel * 4;
      const change = dither ? (BAYER4[(y & 3) * 4 + (x & 3)] - 7.5) * 1.6 : 0;
      const r = (Math.max(0, Math.min(255, rgba[offset] + change)) >>> 3);
      const g = (Math.max(0, Math.min(255, rgba[offset + 1] + change)) >>> 3);
      const b = (Math.max(0, Math.min(255, rgba[offset + 2] + change)) >>> 3);
      indices[pixel] = lookup[(r << 10) | (g << 5) | b];
    }
  }
  return indices;
}

export function packNibbles(indices) {
  if (indices.length & 1) throw new Error('The image has an odd number of pixels.');
  const result = new Uint8Array(indices.length / 2);
  for (let i = 0; i < result.length; i++) {
    const left = indices[i * 2], right = indices[i * 2 + 1];
    if (left > 15 || right > 15) throw new Error('Picture color is outside the palette.');
    result[i] = (left << 4) | right;
  }
  return result;
}

export function buildHeader({ frames, fps, palette, coverPixels }) {
  if (!Number.isSafeInteger(frames) || frames < 1 || frames > 0xffffffff
      || !Number.isInteger(fps) || fps < 1 || fps > 60 || palette.length !== 16)
    throw new Error('Invalid movie timing or palette.');
  const bytes = new Uint8Array(HEADER_BYTES);
  const view = new DataView(bytes.buffer);
  bytes.set([67, 73, 78, 50], 0); // CIN2
  bytes[4] = 2;
  bytes[5] = 1; // packed 4-bit frames
  view.setUint16(6, WIDTH, true);
  view.setUint16(8, HEIGHT, true);
  view.setUint32(10, fps, true);
  view.setUint32(14, 1, true);
  view.setUint32(18, frames, true);
  view.setUint32(22, crc32(bytes.subarray(0, 22)), true);
  for (let i = 0; i < 16; i++) view.setUint16(26 + i * 2, palette[i], true);

  if (coverPixels) {
    if (coverPixels.length !== COVER_W * COVER_H) throw new Error('Invalid cover image.');
    bytes.set([67, 86, 82, 49], 384); // CVR1
    const packed = packNibbles(coverPixels);
    bytes.set(packed, 392);
    view.setUint32(388, crc32(packed), true);
  }
  return bytes;
}

export function movieFilename(filename) {
  const base = filename.replace(/\.[^.]*$/, '').toUpperCase().replace(/[^A-Z0-9]/g, '');
  return `${(base || 'MOVIE').slice(0, 8)}.BIN`;
}

export function expectedSize(frames) {
  return HEADER_BYTES + frames * FRAME_BYTES;
}

export function movieId(filename, frames, fps, bytes) {
  const encoded = new TextEncoder().encode(filename.toUpperCase());
  const data = new Uint8Array(16);
  const view = new DataView(data.buffer);
  view.setUint32(0, frames, true);
  view.setUint32(4, fps, true);
  view.setUint32(8, 1, true);
  view.setUint32(12, bytes, true);
  return crc32(data, crc32(encoded));
}

function parseTime(time) {
  const m = /^(\d+):(\d\d):(\d\d)[,.](\d{3})$/.exec(time);
  if (!m) throw new Error('One of the subtitle times is not readable.');
  return ((Number(m[1]) * 60 + Number(m[2])) * 60 + Number(m[3])) * 1000 + Number(m[4]);
}

function cleanSubtitle(value) {
  return value.replace(/<[^>]*>|\{\\[^}]*\}/g, '')
    .replace(/[\u2018\u2019]/g, "'")
    .replace(/[\u201c\u201d]/g, '"')
    .replace(/[\u2013\u2014]/g, '-')
    .replace(/\u2026/g, '...').replace(/\u00a0/g, ' ')
    .normalize('NFKD').replace(/[\u0300-\u036f]/g, '')
    .replace(/[^\x20-\x7e]/g, '?').replace(/\s+/g, ' ').trim();
}

function wrapSubtitle(text) {
  if (text.length <= LINE_MAX) return [text];
  const words = text.split(' ');
  if (words.some(word => word.length > LINE_MAX))
    throw new Error('A subtitle has a word too long for the calculator screen.');
  let chosen = null;
  for (let i = 1; i < words.length; i++) {
    const first = words.slice(0, i).join(' ');
    const second = words.slice(i).join(' ');
    if (first.length > LINE_MAX || second.length > LINE_MAX) continue;
    if (/^[,.;:!?\])}]/.test(second) || /[([{]$/.test(first)) continue;
    const score = Math.abs(first.length - second.length);
    if (!chosen || score < chosen.score) chosen = { first, second, score };
  }
  if (!chosen) throw new Error('A subtitle cannot fit on two calculator lines.');
  return [chosen.first, chosen.second];
}

export function parseSrt(text) {
  const cleaned = text.replace(/^\ufeff/, '').replace(/\r\n?/g, '\n').trim();
  if (!cleaned) return [];
  return cleaned.split(/\n[ \t]*\n/).map((block, index) => {
    const lines = block.split('\n');
    if (/^\d+$/.test(lines[0].trim())) lines.shift();
    const match = /^(\S+)\s*-->\s*(\S+)$/.exec(lines[0]?.trim() || '');
    if (!match || lines.length < 2) throw new Error(`Subtitle ${index + 1} has an invalid time or missing text.`);
    const start = parseTime(match[1]), end = parseTime(match[2]);
    if (end <= start) throw new Error(`Subtitle ${index + 1} ends before it starts.`);
    return { start, end, lines: wrapSubtitle(cleanSubtitle(lines.slice(1).join(' '))) };
  });
}

export function buildSubtitles(cues, { filename, frames, fps, fileSize }) {
  const records = [];
  let previousEnd = 0;
  const toFrame = milliseconds => Math.floor((milliseconds * fps + 500) / 1000);
  for (const cue of cues) {
    const start = toFrame(cue.start);
    const end = Math.min(frames, toFrame(cue.end));
    if (start >= frames) continue;
    if (start < previousEnd || end <= start)
      throw new Error('Some subtitles overlap or are too short for the selected frame rate.');
    const item = new Uint8Array(72);
    const view = new DataView(item.buffer);
    view.setUint32(0, start, true);
    view.setUint32(4, end, true);
    item[8] = cue.lines.length;
    item[9] = cue.lines[0].length;
    item[10] = cue.lines[1]?.length || 0;
    item.set(new TextEncoder().encode(cue.lines[0]), 12);
    if (cue.lines.length > 1) item.set(new TextEncoder().encode(cue.lines[1]), 42);
    previousEnd = end;
    records.push(item);
  }
  const output = new Uint8Array(40 + records.length * 72);
  const view = new DataView(output.buffer);
  output.set([67, 83, 85, 49], 0); // CSU1
  output[4] = 1;
  view.setUint16(6, 40, true);
  view.setUint16(8, 72, true);
  view.setUint32(12, records.length, true);
  view.setUint32(16, movieId(filename, frames, fps, fileSize), true);
  view.setUint32(20, frames, true);
  view.setUint32(24, fps, true);
  view.setUint32(28, 1, true);
  records.forEach((record, i) => output.set(record, 40 + i * 72));
  view.setUint32(32, crc32(output), true);
  return output;
}
