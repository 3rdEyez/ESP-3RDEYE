"""Run inline browser code against a synthetic DOM/fetch; no HTTP or device."""
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
@unittest.skipUnless(shutil.which('node'),'Browser contract needs Node; no installation performed')
class Tests(unittest.TestCase):
    def test_layout_required_before_post_and_confirmation(self):
        source=(ROOT/'main/ota/wifi_ota_service.cpp').read_text()
        script=re.search(r'R"HTML\(.*?<script>(.*?)</script>',source,re.S)[1]
        harness=r'''
const assert=require('node:assert/strict'),vm=require('node:vm');
(async()=>{
for(const scenario of ['factory','unknown','missing','dual','cancel']){
 const requests=[],header=Buffer.from(JSON.stringify({image_length:1,version:'0.2.8'}));
 const length=Buffer.alloc(4);length.writeUInt32LE(header.length);
 const blob=new Blob([length,header,Buffer.from([1])]);
 const nodes={file:{files:[blob]},install:{disabled:false},status:{textContent:''},token:{value:''}};
 const context={document:{getElementById:id=>nodes[id]},Blob,DataView,Error,confirm:()=>scenario!=='cancel',
  fetch:async(url,options)=>{requests.push(url);
   if(url==='/v1/ota/layout')return {ok:scenario!=='missing',json:async()=>{if(scenario==='missing')throw Error('absent');return {layout:scenario==='dual'||scenario==='cancel'?'dual-ota':scenario,update_allowed:scenario==='dual'||scenario==='cancel'};}};
   return {ok:true,text:async()=>'synthetic accepted'};
  }};
 vm.runInNewContext(SCRIPT,context);await nodes.install.onclick();
 assert.equal(requests.filter(x=>x==='/v1/ota/image').length,scenario==='dual'?1:0);
 assert.equal(nodes.install.disabled,false);
 if(['factory','unknown','missing'].includes(scenario))assert.match(nodes.status.textContent,/双槽迁移/);
}
})();
'''.replace('SCRIPT',json.dumps(script,ensure_ascii=False))
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'browser.cjs';path.write_text(harness)
            result=subprocess.run([shutil.which('node'),str(path)],capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stderr)
