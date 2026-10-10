const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const { createHash } = require('node:crypto');
const root = path.resolve(__dirname, '..');
const variant = process.argv[2] || 'cpu-specialized';
const output = path.join(root, 'build/research',process.argv[2] ? variant+'-checkpoint-diagnostic' : 'cpu-checkpoint-diagnostic');
fs.mkdirSync(output, { recursive: true });
const pinned = path.resolve(root, '../rebit-client/public/cores/melonds_dual_cooperative');
const candidate = path.join(root, 'build/research',variant,'core');
const rom = fs.readFileSync(path.resolve(root, '../rebit/public/test-roms/Mario-Kart-DS-USA.nds'));
const checkpoint = fs.readFileSync(path.resolve(root, '../rebit-client/artifacts/nds-review-2026-10-03/race.bin'));
const sha = bytes => createHash('sha256').update(bytes).digest('hex');
async function trace(name, base) {
    const js = path.join(output, name + '.cjs');
    fs.copyFileSync(base + '.js', js);
    const core = await require(js)({wasmBinary:fs.readFileSync(base+'.wasm'),print(){},printErr(){}});
    const copy = bytes => {const p=core._malloc(bytes.length);assert.ok(p);core.HEAPU8.set(bytes,p);return p;};
    const rp=copy(rom), cp=copy(checkpoint);
    assert.equal(core._md_load(rp,rom.length,2,0x12345678,0x9abcdef0),1);
    core._md_set_visible_player(0);
    assert.equal(core._md_import_checkpoint(cp,checkpoint.length),1);core._free(cp);
    const samples=[];
    for(let step=0;step<=120;++step) {
        let audio=Buffer.alloc(0);
        if(step) {
            for(let p=0;p<2;++p) core._md_set_input(p,0xfff^(1|((step+p*23)%90<30?16:0)),0,0,0);
            assert.equal(core._md_run_frame(),1);const n=core._md_audio_read(4096);
            audio=core.HEAPU8.slice(core._md_audio_buffer(),core._md_audio_buffer()+n*4);
        }
        if(![0,1,120].includes(step))continue;
        assert.equal(core._md_export_checkpoint(),1);
        const state=Buffer.from(core.HEAPU8.slice(core._md_checkpoint_data(),core._md_checkpoint_data()+core._md_checkpoint_size()));
        fs.writeFileSync(path.join(output,`${name}-${step}.bin`),state);
        const ptr=core._md_framebuffer(0);
        samples.push({step,frame:core._md_frame(0),ram:core._md_state_hash()>>>0,
            audio:sha(audio),pixels:sha(core.HEAPU8.subarray(ptr,ptr+256*384*4)),
            saves:[0,1].map(view=>{const n=core._md_save_size(view),p=core._md_save_data(view);return sha(core.HEAPU8.subarray(p,p+n));})});
        core._md_clear_checkpoint();
        console.log(name,step,sha(state));
    }
    core._md_destroy();core._free(rp);
    return samples;
}
function sections(bytes) {
    const result=[{name:'container',offset:0,length:24}];
    let at=24;
    for(let view=0;view<bytes.readUInt32LE(8);++view) {
        const length=bytes.readUInt32LE(at+68),start=at+72;
        result.push({name:`view${view}-metadata`,offset:at,length:88});
        assert.equal(bytes.toString('ascii',start,start+4),'MELN');
        for(let p=start+16;p<start+length;) {
            const size=bytes.readUInt32LE(p+4);assert.ok(size>=16&&p+size<=start+length);
            result.push({name:`view${view}-${bytes.toString('ascii',p,p+4)}`,offset:p,length:size});p+=size;
        }
        at=start+length;
    }
    result.push({name:'radio',offset:at,length:bytes.length-at});return result;
}
(async()=>{
    const reference=await trace('pinned',pinned),actual=await trace('candidate',candidate);
    assert.deepEqual(actual,reference,'Guest/output samples changed');
    const results=[];
    for(const step of [0,1,120]) {
        const a=fs.readFileSync(path.join(output,`pinned-${step}.bin`)),b=fs.readFileSync(path.join(output,`candidate-${step}.bin`));
        assert.equal(a.length,b.length);const differences=[];
        for(const section of sections(a)) {
            let count=0;const first=[];
            for(let i=section.offset;i<section.offset+section.length;++i)if(a[i]!==b[i]){
                ++count;if(first.length<12)first.push({relative:i-section.offset,expected:a[i],actual:b[i]});
                if(variant==='cpu-control') {
                    const relative=i-section.offset;
                    const fifo=relative>=28 && relative<28+256*8 && (relative-28)%8>=5;
                    const pipe=relative>=2088 && relative<2088+4*8 && (relative-2088)%8>=5;
                    assert.ok(section.name.endsWith('-GP3D')&&(fifo||pipe),'Unexpected changed field');
                    assert.equal(b[i],0,'Changed FIFO reserved bytes must be initialized to zero');
                }
            }
            if(count)differences.push({...section,count,first});
        }
        results.push({step,differences});
    }
    fs.writeFileSync(path.join(output,'report.json'),JSON.stringify(results,null,2)+'\n');
    fs.writeFileSync(path.join(output,'outputs.json'),JSON.stringify({ok:true,reference,actual},null,2)+'\n');
    console.log(JSON.stringify(results,null,2));
})().catch(e=>{console.error(e);process.exitCode=1;});
