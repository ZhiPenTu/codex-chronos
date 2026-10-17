import json,base64,pathlib,os,shutil
from playwright.sync_api import sync_playwright
html=(pathlib.Path(__file__).parent/'dashboard.html').read_text(encoding='utf-8')
browser_candidates=[os.getenv('CODEX_TZ_CHROMIUM',''),shutil.which('chromium') or '',shutil.which('chromium-browser') or '',r'C:\Program Files\Google\Chrome\Application\chrome.exe',r'C:\Program Files\Microsoft\Edge\Application\msedge.exe']
browser_path=next((p for p in browser_candidates if p and pathlib.Path(p).exists()),None)
with sync_playwright() as p:
 b=p.chromium.launch(headless=True,executable_path=browser_path,args=['--no-sandbox'])
 for stored in [True,False]:
  page=b.new_page()
  page.add_init_script("window.__intervals=[];window.setInterval=(fn,ms)=>{window.__intervals.push({fn,ms});return window.__intervals.length};window.clearInterval=()=>{};")
  page.goto('about:blank')
  page.evaluate('''() => {window.__store={};Object.defineProperty(window,'localStorage',{value:{getItem:k=>window.__store[k]??null,setItem:(k,v)=>window.__store[k]=v,removeItem:k=>delete window.__store[k],clear:()=>window.__store={}}});}''')
  if stored:
   raw=json.dumps({'state':{'managementKey':'test-secret-key','rememberPassword':True}})
   page.evaluate('''raw=>{const key=new TextEncoder().encode('cli-proxy-api-webui::secure-storage|'+location.host+'|'+navigator.userAgent);const bytes=new TextEncoder().encode(raw);let s='';for(let i=0;i<bytes.length;i++)s+=String.fromCharCode(bytes[i]^key[i%key.length]);localStorage.setItem('cli-proxy-auth','enc::v1::'+btoa(s));}''',raw)
  page.evaluate('''() => {window.__calls=[];window.__urls=[];window.fetch=async(url,opt)=>{window.__calls.push(opt.headers.Authorization);window.__urls.push(url);const data=url.endsWith('/status')?{ok:true,available:true,timezone:'America/Los_Angeles',settings:{enabled:true,logging:true},age_seconds:1}:url.endsWith('/logs')?{events:[]}:url.endsWith('/stats')?{success:0,failure:0}:url.endsWith('/failures')?{records:[]}:{records:[]};return {ok:true,json:async()=>data};};}''')
  page.set_content(html)
  if not stored:
   page.wait_for_timeout(250)
   assert page.locator('#zone').inner_text()=='—'
   page.locator('#management-key').fill('test-secret-key')
   page.locator('#connect').click()
  page.locator('#zone').get_by_text('America/Los_Angeles').wait_for(timeout=5000)
  assert page.evaluate("window.__calls.includes('Bearer test-secret-key')")
  if not stored:
   assert page.evaluate("localStorage.getItem('cli-proxy-auth')") is None
   plugin_credential=page.evaluate("localStorage.getItem('codex-chronos-auth')")
   assert plugin_credential.startswith('enc::v1::')
   assert 'test-secret-key' not in plugin_credential
   page.locator('#remember-key').uncheck()
   assert page.evaluate("localStorage.getItem('codex-chronos-auth')") is None
   assert page.evaluate("window.__intervals.some(item=>item.ms===900000)")
   page.locator('#refresh-interval').select_option('300')
   assert page.evaluate("window.__intervals.some(item=>item.ms===300000)")
   page.evaluate("window.__intervals.at(-1).fn()")
   page.wait_for_timeout(50)
   assert page.evaluate("window.__urls.some(url=>url.endsWith('/egress/refresh'))")
  page.close()
 print('PASS: credential reuse; remember-password storage; automatic egress refresh; opt-out clearing')
 b.close()
