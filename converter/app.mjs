import { Input, BlobSource, ALL_FORMATS, VideoSampleSink } from './vendor/mediabunny.min.mjs';
import {
  WIDTH, HEIGHT, FRAME_BYTES, buildHeader, buildSubtitles, createColorLookup,
  createPalette, expectedSize, movieFilename, packNibbles, parseSrt, quantizeIndices,
} from './codec.mjs';

const $ = id => document.getElementById(id);
const nodes = {
  file: $('video-file'), subtitles: $('subtitle-file'), fps: $('fps'), fit: $('fit'),
  dither: $('dither'), button: $('convert'), cancel: $('cancel'),
  progress: $('progress'), progressArea: $('progress-area'), percent: $('percent'),
  status: $('status'), estimate: $('estimate'), result: $('result'),
  preview: $('preview'), previewCaption: $('preview-caption'),
  drop: $('drop-area'), dropTitle: $('drop-title'), dropSubtitle: $('drop-subtitle'),
};

const view = nodes.preview.getContext('2d', { willReadFrequently: true });
view.imageSmoothingEnabled = true;
let selected = null;
let converting = false;
let cancelRequested = false;
let resultUrl = null;
const BUFFERED_LIMIT = 128 * 1024 * 1024;
const MAX_MOVIE_BYTES = 0xffffffff;

function message(text, isError = false) {
  nodes.result.hidden = false;
  nodes.result.classList.toggle('error', isError);
  nodes.result.textContent = text;
}

function clearMessage() {
  nodes.result.hidden = true;
  nodes.result.classList.remove('error');
  nodes.result.replaceChildren();
}

function readableBytes(bytes) {
  if (bytes >= 1024 ** 3) return `${(bytes / 1024 ** 3).toFixed(2)} GB`;
  return `${(bytes / 1024 ** 2).toFixed(1)} MB`;
}

function describeProgress(label, done, total, paletteStage = false) {
  const percent = Math.min(100, Math.round(
    paletteStage ? done * 10 / Math.max(1, total)
      : 10 + done * 90 / Math.max(1, total)));
  nodes.progressArea.hidden = false;
  nodes.status.textContent = label;
  nodes.progress.value = percent;
  nodes.percent.textContent = `${percent}%`;
}

function dimensionsFit(w, h, mode) {
  const factor = mode === 'cover' ? Math.max(WIDTH / w, HEIGHT / h)
    : Math.min(WIDTH / w, HEIGHT / h);
  const width = w * factor, height = h * factor;
  return [(WIDTH - width) / 2, (HEIGHT - height) / 2, width, height];
}

function drawSample(sample, width, height, mode, canvas = nodes.preview) {
  const ctx = canvas === nodes.preview ? view
    : canvas.getContext('2d', { willReadFrequently: true });
  ctx.fillStyle = '#000';
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  const [x, y, w, h] = dimensionsFit(width, height, mode);
  sample.draw(ctx, x, y, w, h);
  return ctx.getImageData(0, 0, canvas.width, canvas.height).data;
}

function estimateSettings() {
  if (!selected) return;
  const fps = Number(nodes.fps.value);
  const count = Math.round(selected.duration * fps);
  const bytes = expectedSize(count);
  const noStreaming = typeof window.showSaveFilePicker !== 'function';
  nodes.estimate.textContent = `About ${readableBytes(bytes)} · ${count.toLocaleString()} frames`
    + (fps >= 20 ? ' · Faster motion may not be smooth on every calculator.' : '')
    + (noStreaming && bytes > BUFFERED_LIMIT
      ? ' · Try a shorter clip in this browser, or use Chrome on a computer to save a full movie.' : '');
  nodes.button.disabled = converting || count < 1 || bytes > MAX_MOVIE_BYTES
    || (noStreaming && bytes > BUFFERED_LIMIT);
}

async function chooseVideo(file) {
  if (converting || !file) return;
  selected = null;
  nodes.button.disabled = true;
  clearMessage();
  nodes.dropTitle.textContent = file.name;
  nodes.dropSubtitle.textContent = 'Reading video details…';
  nodes.previewCaption.textContent = 'Preparing preview…';
  try {
    if (!('VideoDecoder' in window))
      throw new Error('This browser cannot read video frames for conversion. Try current Chrome or Edge on a computer.');
    const input = new Input({ formats: ALL_FORMATS, source: new BlobSource(file) });
    const track = await input.getPrimaryVideoTrack();
    if (!track || !await track.canDecode())
      throw new Error('This video format is not supported by your browser. Try MP4 with H.264 video.');
    const duration = await track.computeDuration();
    const firstTimestamp = await track.getFirstTimestamp();
    const width = await track.getDisplayWidth();
    const height = await track.getDisplayHeight();
    if (!Number.isFinite(duration) || duration <= 0 || !width || !height)
      throw new Error('Could not read the length or size of this video.');
    const sink = new VideoSampleSink(track);
    const sample = await sink.getSample(firstTimestamp + Math.min(duration / 4, 5));
    if (!sample) throw new Error('Could not decode a preview from this video.');
    try { drawSample(sample, width, height, nodes.fit.value); }
    finally { sample.close(); }
    selected = { file, track, duration, firstTimestamp, width, height };
    nodes.dropSubtitle.textContent = `${(duration / 60).toFixed(1)} min · ${width} × ${height}`;
    nodes.previewCaption.textContent = 'A glimpse of how your video will fit on the screen.';
    estimateSettings();
  } catch (error) {
    nodes.dropSubtitle.textContent = 'Choose another video';
    nodes.previewCaption.textContent = 'Could not show a preview.';
    message(error.message || String(error), true);
  }
}

function previewColors(rgba, palette, lookup, dither) {
  const indices = quantizeIndices(rgba, WIDTH, HEIGHT, lookup, dither);
  const output = new ImageData(WIDTH, HEIGHT);
  for (let i = 0; i < indices.length; i++) {
    const color = palette[indices[i]];
    const r = (color >> 10) & 31, g = (color >> 5) & 31, b = color & 31;
    output.data[i * 4] = (r << 3) | (r >> 2);
    output.data[i * 4 + 1] = (g << 3) | (g >> 2);
    output.data[i * 4 + 2] = (b << 3) | (b >> 2);
    output.data[i * 4 + 3] = 255;
  }
  view.putImageData(output, 0, 0);
}

function startDownload(bytes, filename) {
  const url = URL.createObjectURL(new Blob([bytes], { type: 'application/octet-stream' }));
  const link = document.createElement('a');
  link.href = url;
  link.download = filename;
  link.textContent = `Download ${filename}`;
  link.addEventListener('click', () => setTimeout(() => URL.revokeObjectURL(url), 60000), { once: true });
  nodes.result.append(' ', link);
  return link;
}

async function startConversion() {
  if (converting || !selected) return;
  const { track, file, duration, firstTimestamp, width, height } = selected;
  const fps = Number(nodes.fps.value);
  const fit = nodes.fit.value;
  const dither = nodes.dither.value === 'stable';
  const frames = Math.round(duration * fps);
  const bytes = expectedSize(frames);
  const filename = movieFilename(file.name);
  const noStreaming = typeof window.showSaveFilePicker !== 'function';
  if (!frames || bytes > MAX_MOVIE_BYTES || (noStreaming && bytes > BUFFERED_LIMIT)) {
    message('This movie is too large for the selected browser. Try a shorter clip or a desktop browser.', true);
    return;
  }

  let subtitles = null;
  try {
    if (nodes.subtitles.files?.[0]) {
      const cues = parseSrt(await nodes.subtitles.files[0].text());
      subtitles = buildSubtitles(cues, { filename, frames, fps, fileSize: bytes });
    }
  } catch (error) {
    message(`Please check your subtitle file: ${error.message}`, true);
    return;
  }

  let handle;
  try {
    /* Pick the destination before decoding: browsers require the picker to
     * open while the Convert button's user gesture is still active. */
    if (!noStreaming) {
      handle = await window.showSaveFilePicker({ suggestedName: filename,
        types: [{ description: 'Cinema movie', accept: { 'application/octet-stream': ['.bin'] } }] });
    }
  } catch (error) {
    if (error.name !== 'AbortError') message(`Could not choose a save location: ${error.message}`, true);
    return;
  }

  converting = true;
  cancelRequested = false;
  clearMessage();
  nodes.button.disabled = true;
  nodes.cancel.hidden = false;
  const originalName = filename;
  let writable = null;
  const chunks = [];
  try {
    if (resultUrl) { URL.revokeObjectURL(resultUrl); resultUrl = null; }
    if (handle) writable = await handle.createWritable();

    describeProgress('Picking the best colors…', 0, 100, true);
    const paletteSink = new VideoSampleSink(track);
    const sampleCount = Math.min(128, Math.max(16, Math.round(duration / 12)));
    const sampleTimes = Array.from({ length: sampleCount }, (_, i) =>
      firstTimestamp + Math.min(duration - .0001, (i + .5) * duration / sampleCount));
    const sampled = [];
    const miniCanvas = document.createElement('canvas');
    miniCanvas.width = WIDTH; miniCanvas.height = HEIGHT;
    for await (const sample of paletteSink.samplesAtTimestamps(sampleTimes)) {
      if (cancelRequested) throw new DOMException('Canceled', 'AbortError');
      if (sample) {
        try { sampled.push(new Uint8ClampedArray(drawSample(sample, width, height, fit, miniCanvas))); }
        finally { sample.close(); }
      }
      if (sampled.length % 8 === 0)
        describeProgress('Picking the best colors…', sampled.length, sampleCount, true);
    }
    if (!sampled.length) throw new Error('Could not read video pictures for conversion.');
    const palette = createPalette(sampled);
    const lookup = createColorLookup(palette);
    previewColors(sampled[Math.floor(sampled.length / 2)], palette, lookup, dither);
    nodes.previewCaption.textContent = 'Preview with the finished movie colors.';

    /* Embed a tiny cover in the existing header. No extra file or USB read. */
    const coverCanvas = document.createElement('canvas');
    coverCanvas.width = 20; coverCanvas.height = 12;
    const coverContext = coverCanvas.getContext('2d', { willReadFrequently: true });
    const posterCanvas = document.createElement('canvas');
    posterCanvas.width = WIDTH; posterCanvas.height = HEIGHT;
    posterCanvas.getContext('2d').putImageData(
      new ImageData(sampled[Math.floor(sampled.length / 2)], WIDTH, HEIGHT), 0, 0);
    coverContext.drawImage(posterCanvas, 0, 0, 20, 12);
    const coverPixels = quantizeIndices(coverContext.getImageData(0, 0, 20, 12).data,
      20, 12, lookup);
    const header = buildHeader({ frames, fps, palette, coverPixels });

    if (writable) await writable.write(new Uint8Array(512));
    const streamingSink = new VideoSampleSink(track);
    const timestamps = function* () {
      for (let i = 0; i < frames; i++) yield firstTimestamp + i / fps;
    };
    let written = 0;
    const writeBatch = [];
    for await (const sample of streamingSink.samplesAtTimestamps(timestamps())) {
      if (cancelRequested) throw new DOMException('Canceled', 'AbortError');
      if (!sample) throw new Error(`Could not read frame ${written + 1} from the video.`);
      let rgba;
      try { rgba = drawSample(sample, width, height, fit, miniCanvas); }
      finally { sample.close(); }
      const packed = packNibbles(quantizeIndices(rgba, WIDTH, HEIGHT, lookup, dither));
      if (packed.length !== FRAME_BYTES) throw new Error('The movie frame has an unexpected size.');
      if (writable) {
        writeBatch.push(packed);
        if (writeBatch.length === 32) {
          await writable.write(new Blob(writeBatch));
          writeBatch.length = 0;
        }
      } else chunks.push(packed);
      written++;
      if (written % 24 === 0 || written === frames) {
        describeProgress('Converting your movie…', written, frames);
        await new Promise(resolve => setTimeout(resolve, 0));
      }
    }
    if (written !== frames) throw new Error('The video ended before every frame was saved.');
    if (cancelRequested) throw new DOMException('Canceled', 'AbortError');
    if (writable && writeBatch.length) await writable.write(new Blob(writeBatch));

    if (writable) {
      await writable.write({ type: 'write', position: 0, data: header });
      await writable.close();
      writable = null;
    } else {
      const blob = new Blob([header, ...chunks], { type: 'application/octet-stream' });
      if (blob.size !== bytes) throw new Error('The finished movie size was not correct.');
      resultUrl = URL.createObjectURL(blob);
    }

    nodes.result.hidden = false;
    nodes.result.textContent = `Your movie is ready · ${readableBytes(bytes)}.`;
    if (resultUrl) {
      const link = document.createElement('a');
      link.href = resultUrl;
      link.download = originalName;
      link.id = 'download-movie';
      link.textContent = `Download ${originalName}`;
      nodes.result.append(' ', link);
      link.click();
    } else {
      nodes.result.append(' Saved to the folder you chose.');
    }
    if (subtitles) {
      startDownload(subtitles, originalName.replace(/\.BIN$/i, '.CSU'));
      nodes.result.append(' Copy both files to your flash drive.');
    }
    describeProgress('All done!', frames, frames);
  } catch (error) {
    if (writable) {
      try { await writable.abort(); } catch { /* avoid hiding the first error */ }
    }
    message(error.name === 'AbortError' ? 'Conversion canceled. No completed movie was saved.'
      : `Conversion stopped: ${error.message || String(error)}`, error.name !== 'AbortError');
  } finally {
    converting = false;
    cancelRequested = false;
    nodes.cancel.hidden = true;
    estimateSettings();
  }
}

nodes.file.addEventListener('change', () => chooseVideo(nodes.file.files?.[0]));
nodes.drop.addEventListener('dragover', event => { event.preventDefault(); nodes.drop.classList.add('dragging'); });
nodes.drop.addEventListener('dragleave', () => nodes.drop.classList.remove('dragging'));
nodes.drop.addEventListener('drop', event => {
  event.preventDefault(); nodes.drop.classList.remove('dragging');
  chooseVideo(Array.from(event.dataTransfer.files).find(file => file.type.startsWith('video/')
    || /\.(mp4|mov|mkv|webm|avi)$/i.test(file.name)));
});
nodes.fps.addEventListener('change', estimateSettings);
nodes.fit.addEventListener('change', () => {
  if (selected) chooseVideo(selected.file);
  estimateSettings();
});
nodes.button.addEventListener('click', startConversion);
nodes.cancel.addEventListener('click', () => { cancelRequested = true; nodes.status.textContent = 'Stopping…'; });
