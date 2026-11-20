#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098
//
// Renders resources/logo/*.svg to PNGs beside them, on a transparent background, HEIGHT px high (a browser
// draws SVG faithfully). xythos.png is the one the plugin embeds
// (tools/embed_logo.py); the others are for the README. xythos.svg is also rendered in two layers for the plugin, the
// same size so they line up: xythos-mark.png, the crop circle alone in white (the UI tints it with the highlight
// colour), and xythos-wordmark.png, the letters alone.
//
// Needs Node and Playwright with Chromium. Run from the repository root: node tools/render_logo.js
const fs = require('fs');
const path = require('path');
const { chromium } = require('playwright');

const DIR = path.join(__dirname, '..', 'resources', 'logo');
const HEIGHT = 320; // the top bar draws it ~70 logical px high, so this covers 4x scaling

(async () => {
    const browser = await chromium.launch();
    const jobs = [];
    for (const name of fs.readdirSync(DIR).filter((f) => f.endsWith('.svg'))) {
        const svg = fs.readFileSync(path.join(DIR, name), 'utf8');
        jobs.push([name.replace(/\.svg$/, '.png'), svg]);
        if (name === 'xythos.svg') {
            // the crop circle is the group with id="mark"; the wordmark is the group after it
            const mark = svg.replace(/<g transform="translate\(121[\s\S]*?<\/g>\s*<\/g>/, '');
            jobs.push(['xythos-mark.png', mark.replace(/url\(#iris\)/g, '#ffffff')]);
            jobs.push(['xythos-wordmark.png', svg.replace(/<g id="mark"[\s\S]*?<\/g>/, '')]);
        }
    }
    for (const [png, svg] of jobs) {
        const [, , vw, vh] = svg.match(/viewBox="([^"]+)"/)[1].split(/\s+/).map(Number);
        const width = Math.round(HEIGHT * vw / vh);
        const page = await browser.newPage({ viewport: { width, height: HEIGHT } });
        await page.setContent(`<html><body style="margin:0;background:transparent">${
            svg.replace('<svg ', `<svg width="${width}" height="${HEIGHT}" `)}</body></html>`);
        const out = path.join(DIR, png);
        await page.screenshot({ path: out, omitBackground: true });
        console.log(`${path.relative(process.cwd(), out)}: ${width}x${HEIGHT}`);
        await page.close();
    }
    await browser.close();
})();
