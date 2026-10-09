import importlib.util
from pathlib import Path
import sys
import unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
spec=importlib.util.spec_from_file_location('rollback_plan',ROOT/'tools/plan_ota_rollback_test.py')
model=importlib.util.module_from_spec(spec);spec.loader.exec_module(model)
class Tests(unittest.TestCase):
    def test_current_confirmed_slot_retained_and_other_selected_then_aborted(self):
        p=model.plan(1,2,798448)
        self.assertFalse(p['hardware_execution_supported'])
        self.assertEqual(p['candidate_write_start'],'0x10000')
        self.assertTrue(p['current_slot_must_not_be_erased'])
        self.assertTrue(p['model_new_pending_aborted_returned_to_current'])
        self.assertEqual(model.plan(0,3,100)['candidate_write_start'],'0x180000')
    def test_inconsistent_or_oversized_plan_rejected(self):
        for args in [(1,1,100),(0,2,100),(1,2,0),(1,2,0x170001),(1,0xffffffff-1,100)]:
            with self.assertRaises(ValueError):model.plan(*args)
if __name__=='__main__':unittest.main()
