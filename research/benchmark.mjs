import { createRequire } from 'node:module';
import { readFile, writeFile, mkdir } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import assert from 'node:assert/strict';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const client = path.resolve(root, '../rebit-client');
const { chromium } = createRequire(path.join(client, 'package.json'))('@playwright/test');
const profiling = process.env.NDS_COOP_PROFILE === '1';
const coldBoot = process.env.NDS_SPEED_BOOT === '1';
const frames = Number(process.env.NDS_SPEED_FRAMES || 720);
assert.ok(Number.isSafeInteger(frames) && frames > 120 && frames % 120 === 0,
    'NDS_SPEED_FRAMES must be a multiple of 120 greater than 120');
const output = path.join(root, 'build/research', process.env.NDS_SPEED_OUTPUT || 'browser');
await mkdir(output, { recursive: true });
const pinned = path.join(client, 'public/cores/melonds_dual_cooperative');
const manifest = JSON.parse(await readFile(pinned + '.manifest.json', 'utf8'));
const sha = bytes => createHash('sha256').update(bytes).digest('hex');
const rom = await readFile(path.join(client, '../rebit/public/test-roms/Mario-Kart-DS-USA.nds'));
const checkpoint = coldBoot ? Buffer.alloc(0) : await readFile(path.join(client, 'artifacts/nds-review-2026-10-03/race.bin'));
const worker = `
const assert = (value, message) => { if (!value) throw new Error(message); };
const hash = async bytes => Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', bytes)), b => b.toString(16).padStart(2, '0')).join('');
self.onmessage = async ({data}) => { try {
    importScripts('/core.js');
    const [wasm, rom, checkpoint] = await Promise.all(['/core.wasm','/rom','/checkpoint'].map(async u => new Uint8Array(await (await fetch(u)).arrayBuffer())));
    const core = await createMelondsDualCooperativeModule({ wasmBinary: wasm, print() {}, printErr() {} });
    const copy = bytes => { const p = core._malloc(bytes.length); assert(p, 'allocation failed'); core.HEAPU8.set(bytes,p); return p; };
    const romPtr = copy(rom), statePtr = checkpoint.length ? copy(checkpoint) : 0;
    assert(core._md_load(romPtr,rom.length,2,0x12345678,0x9abcdef0) === 1, 'load failed');
    core._md_set_visible_player(data.seat);
    if (statePtr) {
        assert(core._md_import_checkpoint(statePtr,checkpoint.length) === 1, 'import failed');
        core._free(statePtr);
    }
    self.postMessage({ready:true});
    await new Promise(resolve => { self.onmessage = resolve; });
    const times = [], checks = [];
    const began = performance.now();
    for (let step=1; step<=${frames}; ++step) {
        for (let p=0;p<2;++p) core._md_set_input(p,0xfff ^ (1 | ((step+p*23)%90<30 ? 16 : 0)),0,0,0);
        const start = performance.now();
        assert(core._md_run_frame() === 1,'frame failed');
        const elapsed = performance.now()-start;
        if (step>120) times.push(elapsed);
        assert(core._md_frame(0) === core._md_frame(1),'console frames diverged');
        const n = core._md_audio_read(4096);
        if (step%120) continue;
        const audio = core.HEAPU8.slice(core._md_audio_buffer(),core._md_audio_buffer()+n*4);
        assert(core._md_export_checkpoint() === 1,'checkpoint failed');
        const state = core.HEAPU8.slice(core._md_checkpoint_data(),core._md_checkpoint_data()+core._md_checkpoint_size());
        core._md_clear_checkpoint();
        const ptr = core._md_framebuffer(data.seat);
        const pixels = core.HEAPU8.slice(ptr,ptr+256*384*4);
        const saves = [0,1].map(p => { const size=core._md_save_size(p), ptr=core._md_save_data(p); return core.HEAPU8.slice(ptr,ptr+size); });
        checks.push({step,frame:core._md_frame(0),ram:core._md_state_hash()>>>0,
            state:await hash(state),pixels:await hash(pixels),audio:await hash(audio),saves:await Promise.all(saves.map(hash))});
    }
    const result = {seat:data.seat,times,checks,wallMs:performance.now()-began,heapBytes:core.HEAPU8.byteLength,
        build:core.UTF8ToString(core._md_build_id()),abi:core.UTF8ToString(core._md_runtime_abi())};
    core._md_destroy();core._free(romPtr);self.postMessage({result});
} catch(e) { self.postMessage({error:e.stack||String(e)}); } };`;
const trials = [];
let reference;
const matrix = JSON.parse(process.env.NDS_SPEED_MATRIX || JSON.stringify([['pinned','a'],['control','a'],['late','a'],['indirect','a'],['indirect','b'],['late','b'],['control','b'],['pinned','b']]));
const referenceVariant = process.env.NDS_SPEED_REFERENCE || 'pinned';
assert.equal(matrix[0]?.[0], referenceVariant, 'The first trial must establish the configured reference');
for (const [variant, repeat] of matrix) {
    const base = variant === 'pinned' ? pinned : path.join(root, 'build/research/' + variant, 'core');
    const js = await readFile(base + '.js'), wasm = await readFile(base + '.wasm');
    if (variant === 'pinned') { assert.equal(sha(js),manifest.jsSha256); assert.equal(sha(wasm),manifest.wasmSha256); }
    const files = new Map([['/core.js',['application/javascript',js]],['/core.wasm',['application/wasm',wasm]],
        ['/worker.js',['application/javascript',worker]],['/rom',['application/octet-stream',rom]],
        ['/checkpoint',['application/octet-stream',checkpoint]],['/',['text/html','<title>Cooperative WASM core research</title>']]]);
    const browser = await chromium.launch({headless:true,args:['--disable-background-timer-throttling','--disable-renderer-backgrounding']});
    try {
        const pages = [];
        for (let seat=0;seat<2;++seat) {
            const page = await browser.newPage(); pages.push(page);
            await page.route('**/*', route => { const entry=files.get(new URL(route.request().url()).pathname); assert.ok(entry); return route.fulfill({contentType:entry[0],body:entry[1]}); });
            await page.goto('https://cooperative-research.invalid');
            await page.evaluate(seat => { window.state={}; window.worker=new Worker('/worker.js'); worker.onmessage=({data})=>Object.assign(state,data); worker.onerror=e=>state.error=e.message; worker.postMessage({seat}); },seat);
        }
        await Promise.all(pages.map(p=>p.waitForFunction(()=>state.ready||state.error,null,{timeout:60000})));
        let cdp, serial=0;
        const pending=new Map(), sessions=[];
        const send=(sessionId,method,params={})=>new Promise((resolve,reject)=>{
            const id=++serial; pending.set(id,{resolve,reject});
            cdp.send('Target.sendMessageToTarget',{sessionId,message:JSON.stringify({id,method,params})}).catch(reject);
        });
        if(profiling) {
            cdp=await browser.newBrowserCDPSession();
            cdp.on('Target.receivedMessageFromTarget',({message})=>{
                const data=JSON.parse(message), p=pending.get(data.id); if(!p) return;
                pending.delete(data.id); data.error ? p.reject(new Error(JSON.stringify(data.error))) : p.resolve(data.result);
            });
            const {targetInfos}=await cdp.send('Target.getTargets');
            for(const target of targetInfos.filter(t=>t.type==='worker')) {
                const {sessionId}=await cdp.send('Target.attachToTarget',{targetId:target.targetId,flatten:false});
                sessions.push(sessionId); await send(sessionId,'Profiler.enable');
                await send(sessionId,'Profiler.setSamplingInterval',{interval:2000}); await send(sessionId,'Profiler.start');
            }
            assert.equal(sessions.length,2);
        }
        await Promise.all(pages.map(p=>p.evaluate(()=>{ if(state.error) throw new Error(state.error); worker.postMessage({go:true}); })));
        await Promise.all(pages.map(p=>p.waitForFunction(()=>state.result||state.error,null,{timeout:Math.max(120000,frames*100)})));
        if(profiling) for(let i=0;i<sessions.length;++i) {
            const {profile}=await send(sessions[i],'Profiler.stop');
            await writeFile(path.join(output,'worker-'+i+'.cpuprofile'),JSON.stringify(profile));
            await cdp.send('Target.detachFromTarget',{sessionId:sessions[i]});
        }
        const results = await Promise.all(pages.map(p=>p.evaluate(()=>state)));
        for (const result of results) assert.equal(result.error,undefined);
        const samples=results.map(r=>r.result);
        const trial={variant,repeat,profiling,browser:browser.version(),wasmSha256:sha(wasm),jsSha256:sha(js),ok:false,players:samples};
        await writeFile(path.join(output, variant+'-'+repeat+'.json'),JSON.stringify(trial,null,2)+'\n');
        if (!reference) reference=samples.map(p=>p.checks);
        samples.forEach((p,i)=>assert.deepEqual(p.checks,reference[i],variant+' changed state/video/audio/save at seat '+i));
        const simulation = p=>p.checks.map(({frame,ram,saves})=>({frame,ram,saves}));
        assert.deepEqual(simulation(samples[0]),simulation(samples[1]),'opposite views diverged');
        trial.ok=true;
        trials.push(trial);
        await writeFile(path.join(output, variant+'-'+repeat+'.json'),JSON.stringify(trial,null,2)+'\n');
        console.log(JSON.stringify({variant,repeat,ok:true,players:samples.map(p=>({meanMs:p.times.reduce((a,b)=>a+b,0)/p.times.length,p95Ms:[...p.times].sort((a,b)=>a-b)[Math.floor(p.times.length*.95)]}))}));
        await writeFile(path.join(output,'report.json'),JSON.stringify({ok:true,referenceVariant,startMode:coldBoot?'cold-boot':'race-checkpoint',scope:`Core-only browser timing; two simultaneous two-console workers; no netcode/pacing/presentation; first 120 frames excluded; ${frames-120} measured frames.`,frames,warmupFrames:120,trials},null,2)+'\n');
    } finally { await browser.close(); }
}
