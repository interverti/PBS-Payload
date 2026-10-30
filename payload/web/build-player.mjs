// SPDX-License-Identifier: GPL-3.0-or-later
import {build} from 'esbuild';
import {mkdir, copyFile, readFile, readdir, writeFile, appendFile} from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
import path from 'node:path';
const root = path.dirname(fileURLToPath(import.meta.url));
await mkdir(path.join(root, 'vendor'), {recursive: true});
const result = await build({
  absWorkingDir: root, entryPoints: ['player.js'],
  outfile: 'vendor/videojs-player.js', bundle: true, format: 'esm',
  minify: true, target: ['safari16.4', 'chrome110', 'firefox115'],
  legalComments: 'eof', metafile: true
});
await copyFile(path.join(root, 'node_modules/@videojs/html/LICENSE'), path.join(root, 'vendor/VIDEOJS-LICENSE'));
const packages = new Set();
for (const output of Object.values(result.metafile.outputs)) {
  for (const [input, usage] of Object.entries(output.inputs)) {
    if (!usage.bytesInOutput) continue;
    const match = input.match(/^node_modules\/(@[^/]+\/[^/]+|[^/]+)\//);
    if (match) packages.add(match[1]);
  }
}
let notices = '';
for (const name of [...packages].sort()) {
  const dir = path.join(root, 'node_modules', name);
  const pkg = JSON.parse(await readFile(path.join(dir, 'package.json'), 'utf8'));
  notices += `\n${name} ${pkg.version}\n`;
  for (const file of (await readdir(dir)).filter(name => /^(license|copying|notice)(\.|$)/i.test(name)))
    notices += `\n${await readFile(path.join(dir, file), 'utf8')}\n`;
}
await writeFile(path.join(root, 'vendor/THIRD-PARTY-LICENSES'), notices);
// Keep redistribution notices in the embedded ELF as well as the source tree.
await appendFile(path.join(root, 'vendor/videojs-player.js'), `\n/* Third-party licenses\n${notices.replaceAll('*/', '* /')}\n*/\n`);
