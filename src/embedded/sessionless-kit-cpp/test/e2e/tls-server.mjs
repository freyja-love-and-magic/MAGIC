// A tiny HTTPS server with a self-signed certificate, for proving the resolver verifies certificates.
// Prints "READY" then one "REQUEST <method> <url>" per request it actually receives.
import https from 'node:https';
import fs from 'node:fs';
const [,, certPath, keyPath, port] = process.argv;
https.createServer({ cert: fs.readFileSync(certPath), key: fs.readFileSync(keyPath) }, (req, res) => {
  console.log(`REQUEST ${req.method} ${req.url}`);
  req.resume();
  res.writeHead(200, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify({ uuid: '11111111-2222-4333-8444-555555555555' }));
}).listen(Number(port), '127.0.0.1', () => console.log('READY'));
