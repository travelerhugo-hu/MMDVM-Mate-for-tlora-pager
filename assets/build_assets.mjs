import { Resvg } from '/Users/hugohu/.workbuddy/binaries/node/workspace/node_modules/@resvg/resvg-js/index.js';
import fs from 'fs';

// ---- Palette (from ui.cpp C_* defines) -------------------------------------
const C = {
  BG: '#0D1117', PANEL: '#161B22', PANEL2: '#1C2129', BORDER: '#30363D',
  TEXT: '#C9D1D9', DIM: '#6E7681', ACCENT: '#58A6FF', OK: '#3FB950',
  WARN: '#D29922', ERR: '#F85149',
};

// ---- The actual device main screen (480x222), built from ui.cpp layout -----
const screen = `
<rect x="0" y="0" width="480" height="222" fill="${C.BG}"/>

<!-- top banner -->
<rect x="0" y="0" width="480" height="30" fill="${C.PANEL}"/>
<g transform="translate(10,7)" stroke="${C.OK}" stroke-width="2" fill="none" stroke-linecap="round">
  <path d="M5 11 A11 11 0 0 1 27 11"/>
  <path d="M9 15 A7 7 0 0 1 23 15"/>
  <circle cx="16" cy="19" r="1.8" fill="${C.OK}" stroke="none"/>
</g>
<text x="34" y="20" font-family="Arial" font-size="14" fill="${C.OK}">HOSELINE</text>
<text x="240" y="20" text-anchor="middle" font-family="Arial" font-size="16" fill="${C.TEXT}">12:34:56 UTC</text>
<g transform="translate(436,9)">
  <rect x="0" y="0" width="16" height="13" rx="2" fill="none" stroke="${C.TEXT}" stroke-width="1.5"/>
  <rect x="16.2" y="3.5" width="2.6" height="6" fill="${C.TEXT}"/>
  <rect x="2" y="2" width="11" height="9" fill="${C.TEXT}"/>
</g>
<text x="432" y="20" text-anchor="end" font-family="Arial" font-size="14" fill="${C.TEXT}">82%</text>

<!-- call panel -->
<text x="10" y="52" font-family="Arial" font-size="20" fill="${C.ACCENT}">TG 2624</text>
<rect x="320" y="40" width="150" height="8" rx="2" fill="${C.PANEL2}"/>
<rect x="320" y="40" width="95" height="8" rx="2" fill="${C.OK}"/>
<text x="240" y="86" text-anchor="middle" font-family="Arial" font-weight="bold" font-size="40" fill="${C.OK}">DL1ABC</text>
<text x="240" y="112" text-anchor="middle" font-family="Arial" font-size="14" fill="${C.TEXT}">Hans M\u00fcller</text>

<!-- history panel -->
<rect x="0" y="120" width="480" height="76" fill="${C.PANEL}"/>
<line x1="0" y1="120" x2="480" y2="120" stroke="${C.BORDER}" stroke-width="1"/>
<text x="10" y="135" font-family="Arial" font-size="14" fill="${C.OK}">DL1ABC</text>
<text x="470" y="135" text-anchor="end" font-family="Arial" font-size="14" fill="${C.DIM}">TG 2624  12:31</text>
<text x="10" y="153" font-family="Arial" font-size="14" fill="${C.TEXT}">OE3DEF</text>
<text x="470" y="153" text-anchor="end" font-family="Arial" font-size="14" fill="${C.DIM}">TG 2624  12:28</text>
<text x="10" y="171" font-family="Arial" font-size="14" fill="${C.TEXT}">F4GHB</text>
<text x="470" y="171" text-anchor="end" font-family="Arial" font-size="14" fill="${C.DIM}">TG 2624  12:21</text>
<text x="10" y="189" font-family="Arial" font-size="14" fill="${C.TEXT}">GM1XYZ</text>
<text x="470" y="189" text-anchor="end" font-family="Arial" font-size="14" fill="${C.DIM}">TG 2624  12:15</text>

<!-- bottom banner -->
<rect x="0" y="196" width="480" height="26" fill="${C.PANEL2}"/>
<text x="8" y="213" font-family="Arial" font-size="12" fill="${C.DIM}">S settings   L backlight   Q/E volume</text>
<g transform="translate(426,205)">
  <path d="M0 1 L4 1 L9 -4 L9 13 L4 8 L0 8 Z" fill="${C.TEXT}"/>
  <path d="M12 1 A5 5 0 0 1 12 11" stroke="${C.TEXT}" stroke-width="1.4" fill="none"/>
  <path d="M15 4 A2.6 2.6 0 0 1 15 8" stroke="${C.TEXT}" stroke-width="1.4" fill="none"/>
</g>
<text x="470" y="213" text-anchor="end" font-family="Arial" font-size="12" fill="${C.TEXT}">80%</text>
`;

// ---- Main UI render (1:1 of the screen) -----------------------------------
const mainSvg = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 480 222" width="480" height="222">${screen}</svg>`;

// ---- Cover (1536x1024) ----------------------------------------------------
const coverSvg = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1536 1024" width="1536" height="1024">
<defs>
  <linearGradient id="bg" x1="0" y1="0" x2="1" y2="1">
    <stop offset="0" stop-color="#0E1633"/>
    <stop offset="1" stop-color="#0E3A3F"/>
  </linearGradient>
</defs>
<rect x="0" y="0" width="1536" height="1024" fill="url(#bg)"/>

<!-- globe motif (right) -->
<g stroke="${C.ACCENT}" fill="none" stroke-opacity="0.30" stroke-width="1.5">
  <circle cx="1200" cy="560" r="150"/>
  <ellipse cx="1200" cy="560" rx="60" ry="150"/>
  <ellipse cx="1200" cy="560" rx="110" ry="150"/>
  <line x1="1050" y1="560" x2="1350" y2="560"/>
  <line x1="1057" y1="480" x2="1343" y2="480"/>
  <line x1="1057" y1="640" x2="1343" y2="640"/>
</g>
<g fill="${C.ACCENT}" fill-opacity="0.5">
  <circle cx="1150" cy="500" r="3"/>
  <circle cx="1260" cy="600" r="3"/>
  <circle cx="1210" cy="520" r="2.5"/>
</g>

<!-- radio waves from device to globe -->
<g stroke="${C.ACCENT}" fill="none" stroke-opacity="0.18" stroke-width="2">
  <path d="M1000 560 A150 150 0 0 1 1180 470"/>
  <path d="M1000 560 A230 230 0 0 1 1240 400"/>
  <path d="M1000 560 A310 310 0 0 1 1270 350"/>
</g>

<!-- device body + screen -->
<rect x="240" y="394" width="760" height="373" rx="30" fill="#0B0E14" stroke="${C.BORDER}" stroke-width="2"/>
<g transform="translate(260,414) scale(1.5)">${screen}</g>

<!-- wordmark -->
<text x="768" y="150" text-anchor="middle" font-family="Arial" font-weight="bold" font-size="64" fill="#FFFFFF">MMDVM Mate</text>
<text x="768" y="200" text-anchor="middle" font-family="Arial" font-size="26" fill="${C.TEXT}">Listen to the world's talkgroups</text>
</svg>`;

const fonts = [
  '/System/Library/Fonts/Supplemental/Arial.ttf',
  '/System/Library/Fonts/Supplemental/Arial Bold.ttf',
];
const opts = { font: { fontFiles: fonts, defaultFontFamily: 'Arial' } };

fs.writeFileSync('ui-mockup-mmdvm-mate.svg', mainSvg);
fs.writeFileSync('cover-mmdvm-mate.svg', coverSvg);

const main = new Resvg(mainSvg, { ...opts, fitTo: { mode: 'width', value: 960 } }).render().asPng();
fs.writeFileSync('ui-mockup-mmdvm-mate.png', main);

const cover = new Resvg(coverSvg, { ...opts, fitTo: { mode: 'width', value: 1536 } }).render().asPng();
fs.writeFileSync('cover-mmdvm-mate.png', cover);

console.log('main png bytes:', main.length);
console.log('cover png bytes:', cover.length);
