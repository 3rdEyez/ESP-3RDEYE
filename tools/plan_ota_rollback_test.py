#!/usr/bin/env python3
"""Offline rollback rehearsal model. No serial, networking or execution mode."""
import argparse
import json
from ota_migration_artifacts import ota_record,selected_slot


def plan(current_slot,current_sequence,fault_image_bytes):
    if current_slot not in (0,1) or (current_sequence-1)%2!=current_slot or current_sequence<1:
        raise ValueError('Current confirmed sequence does not identify its slot')
    if not 1<=fault_image_bytes<=0x170000:raise ValueError('Fault image outside slot bounds')
    candidate_sequence=current_sequence+1
    if candidate_sequence>=0xffffffff:raise ValueError('Sequence near rollover; separate review required')
    data=bytearray(b'\xff'*8192)
    # This is the proposed record model only, not a read or write of a device.
    current_record=(current_sequence-1)%2
    candidate_record=1-current_record
    data[current_record*4096:current_record*4096+32]=ota_record(current_sequence,2)
    data[candidate_record*4096:candidate_record*4096+32]=ota_record(candidate_sequence,0)
    assert selected_slot(data)==1-current_slot
    data[candidate_record*4096:candidate_record*4096+32]=ota_record(candidate_sequence,1)
    assert selected_slot(data)==1-current_slot
    data[candidate_record*4096:candidate_record*4096+32]=ota_record(candidate_sequence,4)
    assert selected_slot(data)==current_slot
    start=0x10000 if current_slot==1 else 0x180000
    return {'mode':'OFFLINE PLAN ONLY','hardware_execution_supported':False,
            'current_known_good_slot':current_slot,'current_slot_must_not_be_erased':True,
            'fault_candidate_slot':1-current_slot,'candidate_write_start':hex(start),
            'candidate_sector_end_exclusive':hex(start+(fault_image_bytes+4095)//4096*4096),
            'otadata_only_other_write':[hex(0x303000),hex(0x305000)],
            'bootloader_table_nvs_phy_config_writes':False,
            'model_new_pending_aborted_returned_to_current':True,
            'required_before_real_test':['explicit authorization for inactive app slot + otadata and deliberate failed startup',
                'known-good current slot VALID and fresh private region backups',
                'fault image uses current authenticity policy; signed candidate/unsigned fallback must not be assumed compatible',
                'exact artifact hash/sector plan and no-output deterministic failure hook',
                'bounded ROM regional recovery and user-confirmed safe mechanical conditions'],
            'proposed_chip_reset_count':5,
            'reset_sequence':['enter ROM','boot fault candidate','intentional rollback reboot',
                              'enter ROM for read-only confirmation','final normal application boot'],
            'if_app_rejects_unsigned_fallback':'Stop; current pending image and existing unsigned bootloader require bounded USB reset/metadata recovery, never blind erase',
            'initial_signed_seed':'Separate transition: keep current unsigned ota_1 0.2.4 intact; no fault injection during first signed installation'}


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--current-slot',type=int,default=1)
    parser.add_argument('--current-sequence',type=int,default=2)
    parser.add_argument('--fault-image-bytes',type=int,default=798448)
    args=parser.parse_args()
    print(json.dumps(plan(args.current_slot,args.current_sequence,args.fault_image_bytes),indent=2))
