#!/usr/bin/env node
import { Identity, verify, fileStore } from '../src/index.js';

const USAGE = `sessionless-kit <command> [--name <identity>] [--path <file>]

  keygen                       create keys (fails if they already exist) and print the public key
  pubkey                       print the public key, creating keys on first use
  sign <message>               print the signature of <message> ('-' reads stdin)
  verify <sig> <message> <pubKey>
                               exit 0 and print "valid", or exit 1 and print "invalid"
  export                       print the private key hex (treat like a password)
  import <privateKey>          save an existing private key

Keys default to ~/.sessionless/<name>.json (SESSIONLESS_HOME overrides the directory).`;

const parse = (argv) => {
  const args = [];
  const opts = {};
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--name' || argv[i] === '--path') {
      if (argv[i + 1] === undefined) throw new Error(`${argv[i]} needs a value`);
      opts[argv[i].slice(2)] = argv[++i];
    } else {
      args.push(argv[i]);
    }
  }
  return { args, opts };
};

const readStdin = async () => {
  const chunks = [];
  for await (const c of process.stdin) chunks.push(c);
  return Buffer.concat(chunks).toString('utf-8').replace(/\n$/, '');
};

const need = (args, n, usage) => {
  if (args.length < n) throw new Error(`usage: sessionless-kit ${usage}`);
};

const main = async () => {
  const { args: [cmd, ...rest], opts } = parse(process.argv.slice(2));
  const storeOpts = { name: opts.name, path: opts.path };

  switch (cmd) {
    case 'keygen': {
      const id = await Identity.create(storeOpts);
      console.error(`Saved to ${fileStore(storeOpts).path}`);
      console.log(id.pubKey);
      break;
    }
    case 'pubkey':
      console.log((await Identity.loadOrCreate(storeOpts)).pubKey);
      break;
    case 'sign': {
      need(rest, 1, 'sign <message>');
      const message = rest[0] === '-' ? await readStdin() : rest[0];
      console.log(await (await Identity.load(storeOpts)).sign(message));
      break;
    }
    case 'verify': {
      need(rest, 3, 'verify <sig> <message> <pubKey>');
      const ok = verify(rest[0], rest[1], rest[2]);
      console.log(ok ? 'valid' : 'invalid');
      process.exitCode = ok ? 0 : 1;
      break;
    }
    case 'export':
      console.log((await Identity.load(storeOpts)).exportPrivateKey());
      break;
    case 'import': {
      need(rest, 1, 'import <privateKey>');
      console.log((await Identity.import(rest[0], storeOpts)).pubKey);
      break;
    }
    default:
      console.error(USAGE);
      process.exitCode = cmd ? 2 : 0;
  }
};

main().catch((err) => {
  console.error(`Error: ${err.message}`);
  process.exitCode = 2;
});
