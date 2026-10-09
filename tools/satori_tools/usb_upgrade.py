"""Normal app-only USB core: no migration, fault injection or signing."""
import binascii,re,signal,struct
import ota_package as package
from . import ota_records as model
from . import ota_records as migration
from . import ota_records as stage
from .runtime import Rejected,LayoutRejected,digest as sha
SLOT=0x170000
FLASH=0x400000
OTADATA=0x303000
def validate_candidate(blob,expected_signed_sha,verify):
    if sha(blob)!=expected_signed_sha:raise Rejected('Candidate SHA mismatch')
    try:metadata=package.image_metadata(blob)
    except (ValueError,UnicodeError):raise Rejected('Candidate header/board/chip/version invalid') from None
    if len(blob)%4096 or len(blob)<8192 or len(blob)>SLOT:raise Rejected('Signed image geometry mismatch')
    block=blob[-4096:-4096+1216]
    if block[:2]!=b'\xe7\x02' or struct.unpack_from('<I',block,1196)[0]!=(binascii.crc32(block[:1196])&0xffffffff):raise Rejected('Signature block structure/CRC invalid')
    if not verify():raise Rejected('Official RSA verification failed')
    return metadata

class Plan:
    def __init__(self,image,current_seed,boot,table,expected_signed_sha,verify,current_slot=0,target_slot=1,expected_current_sha=None):
        if current_slot not in (0,1) or target_slot not in (0,1) or target_slot==current_slot:raise Rejected('Wrong or active target slot')
        if expected_current_sha is None or sha(current_seed)!=expected_current_sha:raise Rejected('Current rollback image identity mismatch')
        self.seed_metadata=package.image_metadata(current_seed)
        model.require_dual(table)
        self.metadata=validate_candidate(image,expected_signed_sha,verify)
        self.image=image;self.seed=current_seed;self.boot=migration.pad_region(boot);self.table=migration.pad_region(table)
        self.current_slot=current_slot;self.target=target_slot;self.start=0x10000+target_slot*SLOT;self.padded=migration.pad_region(image)
        if len(self.padded)>SLOT:raise Rejected('Target overflow')
    def public(self):return {'mode':'normal-lan','candidate_version':self.metadata['version'],'candidate_sha256':sha(self.image),'candidate_bytes':len(self.image),'expected_running_version':self.seed_metadata['version'],'expected_running_slot':self.current_slot,'target_slot':self.target,'app_write_address':hex(self.start),'app_write_bytes':len(self.padded),'otadata_address':hex(OTADATA),'otadata_bytes':4096,'otadata_readback_bytes':8192,'bootloader_table_nvs_config_written':False,'fresh_double_backup_required':True,'recovery':'restore exact former otadata only after protected regions verified; preserve signed seed','signature_verified':True,'workflow_deadline_seconds':900,'recovery_deadline_seconds':300}
    def validate_current(self,first,second):
        if len(first)!=FLASH or first!=second:raise Rejected('Fresh double backup mismatch')
        model.require_dual(first[0x8000:0x8c00])
        if first[:len(self.boot)]!=self.boot or first[0x8000:0x8000+len(self.table)]!=self.table:raise Rejected('Immutable bootloader/table differs')
        old=first[OTADATA:OTADATA+8192];records=stage.records(old)
        eligible=[r for r in records if r[2]]
        if not eligible or max(eligible,key=lambda r:r[0])[1]!=2 or model.selected_slot(old)!=self.current_slot:raise Rejected('Actual selected slot is not expected VALID baseline')
        for offset in (0,4096):
            raw=old[offset:offset+32]
            if raw==b'\xff'*32:continue
            seq,_,state,_=struct.unpack('<I20sII',raw)
            if seq in (0,0xffffffff) or state not in (0,1,2,3,4) or raw!=model.ota_record(seq,state):raise Rejected('Unexpected or invalid otadata record')
        start=0x10000+self.current_slot*SLOT
        if first[start:start+len(self.seed)]!=self.seed:raise Rejected('Current app does not match reviewed rollback image')
        if self.image[36:40]!=self.seed[36:40] or self.image[80:112]!=self.seed[80:112]:raise Rejected('Project/security version differs')
        new,seq=stage.selection(old,self.target)
        winner=max(eligible,key=lambda r:r[0]);index=records.index(winner)
        if new[index*4096:(index+1)*4096]!=old[index*4096:(index+1)*4096]:raise Rejected('VALID fallback selection was altered')
        return old,new,seq
    def expected(self,current,newota=None):
        out=bytearray(current);out[self.start:self.start+len(self.padded)]=self.padded
        if newota is not None:out[OTADATA:OTADATA+8192]=newota
        return bytes(out)
    def protected_match(self,current,baseline):
        if len(current)!=FLASH:return False
        allowed=[(self.start,self.start+len(self.padded)),(OTADATA,OTADATA+8192),(0x9000,0x10000)]
        cursor=0
        for start,end in sorted(allowed):
            if current[cursor:start]!=baseline[cursor:start]:return False
            cursor=end
        return current[cursor:]==baseline[cursor:]
    def boot_ok(self,lines,pending,expected_version=None,expected_slot=None):
        # Serial observation starts just after the explicitly requested reset.
        # The initial ROM banner may be missed, but a second boot boundary is
        # never expected. Link version/slot/confirmation to one app_main run.
        expected_version=expected_version or self.metadata['version']
        rom=[i for i,x in enumerate(lines) if 'ESP-ROM:' in x]
        resets=[i for i,x in enumerate(lines) if re.search(r'^rst:0x',x)]
        mains=[i for i,x in enumerate(lines) if 'APP MAIN: Boot reset_reason=' in x or re.search(r'\bBoot reset_reason=',x)]
        if len(rom)>1 or len(resets)>1 or len(mains)!=1:return False
        main=mains[0]
        if any(i>main for i in rom+resets):return False
        if any(re.search(r'Guru Meditation|panic(?:ked)?|Backtrace:|abort\(\)|assert failed|Stack protection fault',x,re.I) for x in lines):return False
        versions=[x for x in lines if re.search(r'\bbuild=',x)]
        states=[x for x in lines if 'running_slot=' in x]
        if len(versions)!=1 or len(states)!=1:return False
        state_index=next(i for i,x in enumerate(lines) if 'running_slot=' in x)
        confirmations=[i for i,x in enumerate(lines) if 'Startup validation passed; slot confirmed' in x]
        if state_index<=main or len(confirmations)!=1 or confirmations[0]<=state_index:return False
        starts=[i for i,x in enumerate(lines) if re.search(r"main_task: (Started|Calling app_main)",x)]
        if any(i>main for i in starts):return False
        slot=16+(self.target if expected_slot is None else expected_slot)
        return (any(re.search(r'\brunning_slot=%d state=%d(?:[;,]|\s|$)'%(slot,1 if pending else 2),x) for x in lines) and
                any(re.search(r'\bbuild=%s(?:[;,]|\s|$)'%re.escape(expected_version),x) for x in lines) and not any('OTA_FAULT' in x for x in lines) and
                bool(confirmations))

def selection_sector(old, new):
    changed = [offset for offset in (0, 4096) if old[offset:offset+4096] != new[offset:offset+4096]]
    if len(changed) != 1:
        raise Rejected('Selection must change exactly one sector')
    return changed[0]

def execute(plan,io,save=lambda meta:None,backup=lambda number,blob:None):
    result={**plan.public(),'success':False,'writes_started':False,'selection_write_started':False,'port_closed':False};baseline=None;oldota=None;old_sigint=None
    def phase(name):result['phase']=name;save(result)
    try:
        phase('open_rom');io.open()
        phase('backup_1');baseline=io.read(0,FLASH);backup(1,baseline)
        phase('backup_2');second=io.read(0,FLASH);backup(2,second)
        phase('validate_baseline');oldota,newota,seq=plan.validate_current(baseline,second);result['backup_sha256']=sha(baseline);result['new_sequence']=seq
        phase('write_inactive_app');result['writes_started']=True;save(result);io.write(plan.start,plan.padded)
        phase('verify_app_before_selection')
        if io.read(0,FLASH)!=plan.expected(baseline):raise Rejected('App write/readback or protected-region comparison failed')
        phase('write_new_selection');result['selection_write_started']=True;save(result);sector=selection_sector(oldota,newota);io.write(OTADATA+sector,newota[sector:sector+4096])
        phase('verify_full_preboot')
        if io.read(0,FLASH)!=plan.expected(baseline,newota):raise Rejected('Selection/full comparison failed')
        phase('first_normal_boot');io.boot()
        if not plan.boot_ok(io.observe(26),True):raise Rejected('New candidate startup confirmation missing')
        phase('verify_actual_valid');io.enter();actual=io.read(0,FLASH);finalota=actual[OTADATA:OTADATA+8192]
        if not plan.protected_match(actual,baseline) or actual[plan.start:plan.start+len(plan.padded)]!=plan.padded:raise Rejected('Postboot protected region/app changed')
        expected_valid=bytearray(newota)
        locations=[i for i in (0,4096) if newota[i:i+32]==model.ota_record(seq,0)]
        if len(locations)!=1:raise Rejected('Planned NEW selection is ambiguous')
        expected_valid[locations[0]:locations[0]+32]=model.ota_record(seq,2)
        if finalota!=bytes(expected_valid) or model.selected_slot(finalota)!=plan.target:raise Rejected('Candidate is not exact CRC-valid VALID selection')
        # Old signed baseline record remains intact for rollback; don't mistake
        # the unrelated ABORTED former candidate for a valid fallback.
        previous=stage.records(oldota);index=previous.index(max((r for r in previous if r[2]),key=lambda r:r[0]))
        if finalota[index*4096:(index+1)*4096]!=oldota[index*4096:(index+1)*4096]:raise Rejected('Fallback record changed')
        phase('second_normal_boot');io.boot()
        if not plan.boot_ok(io.observe(20),False):raise Rejected('Second VALID/version boot missing')
        result['success']=True;result['phase']='complete'
    except (Exception,KeyboardInterrupt) as error:
        # A cooperative Ctrl+C gets the same bounded rollback as transport
        # failure. Further Ctrl+C cannot interrupt this single recovery/cleanup.
        old_sigint=signal.getsignal(signal.SIGINT);signal.signal(signal.SIGINT,signal.SIG_IGN)
        if isinstance(error,LayoutRejected):result['guidance']=str(error)
        result['interrupted']=isinstance(error,KeyboardInterrupt)
        result['failed_phase']=result.get('phase');result['error_type']=type(error).__name__;result['failure_reason']=str(error) if isinstance(error,Rejected) else ('User interrupted deployment' if isinstance(error,KeyboardInterrupt) else 'Transport operation failed')
        if baseline is not None and result['writes_started']:
            try:
                if hasattr(io,'recovery_bound'):io.recovery_bound()
                phase('recovery_verify_protected');io.enter();now=io.read(0,FLASH)
                if not plan.protected_match(now,baseline):raise Rejected('Protected-region mismatch; automatic recovery refused')
                if result['selection_write_started']:
                    sector=selection_sector(oldota,newota);fallback=4096-sector
                    if now[OTADATA+fallback:OTADATA+fallback+4096]!=oldota[fallback:fallback+4096]:raise Rejected('VALID fallback changed; recovery refused')
                    phase('recovery_restore_old_selection');io.write(OTADATA+sector,oldota[sector:sector+4096])
                    if io.read(OTADATA,8192)!=oldota:raise Rejected('Recovery selection readback failed')
                phase('recovery_boot_old_seed');io.boot();lines=io.observe(20)
                result['recovery_confirmed']=plan.boot_ok(lines,False,plan.seed_metadata['version'],plan.current_slot)
                if not result['recovery_confirmed']:raise Rejected('Recovery normal startup missing')
            except Exception as recovery:result['recovery_error_type']=type(recovery).__name__;result['recovery_confirmed']=False
        elif io.opened:
            # No flash write happened. Leave ROM by returning to the unchanged app.
            try:io.boot();io.observe(20);result['unchanged_boot_requested']=True
            except Exception as cleanup:result['cleanup_error_type']=type(cleanup).__name__
    finally:
        try:io.close();result['port_closed']=True
        except Exception as error:result['close_error_type']=type(error).__name__
        result['left_in_rom']=getattr(io,'in_rom',None)
        try:save(result)
        finally:
            if old_sigint is not None:signal.signal(signal.SIGINT,old_sigint)
    return result
