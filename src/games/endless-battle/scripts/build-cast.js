// Bundles the viewer's cast page (and the sessionless-kit browser build it signs with) into one script.
import { build } from 'esbuild';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
await build({
  entryPoints: [path.join(root, 'public/js/cast-src.js')],
  outfile: path.join(root, 'public/js/cast.bundle.js'),
  bundle: true, format: 'iife', platform: 'browser', target: 'es2020', minify: true, logLevel: 'warning'
});
console.log('built public/js/cast.bundle.js');
