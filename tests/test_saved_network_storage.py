import subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class StorageTests(unittest.TestCase):
 def test_production_deadline_reads_flag_then_timestamp_then_clock(self):
  source=(ROOT/'main/ota/wifi_ota_service.cpp').read_text()
  start=source.index('std::uint32_t Remaining(){');end=source.index('\n}',start)+2
  function=source[start:end]
  self.assertLess(source.index('upload_started_ms.store(Now()'),source.index('uploading.store(true'))
  harness=r'''#include <atomic>
#include <vector>
#include <cassert>
#include "maintenance_lifetime.hpp"
using namespace satori::ota;
std::vector<int> reads;
struct Flag{bool value;bool load(std::memory_order){reads.push_back(0);return value;}} uploading{true};
struct Stamp{int id;unsigned value;unsigned load(std::memory_order){reads.push_back(id);return value;}} upload_started_ms{1,599000},started_ms{3,0};
unsigned Now(){reads.push_back(2);return 610000;}
'''+function+r'''
int main(){assert(Remaining()==109000);assert((reads==std::vector<int>{0,1,2}));reads.clear();uploading.value=false;assert(Remaining()==0);assert((reads==std::vector<int>{0,3,2}));}
'''
  with tempfile.TemporaryDirectory() as directory:
   cpp=Path(directory)/'test.cpp';binary=Path(directory)/'test';cpp.write_text(harness)
   subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(ROOT/'main/ota'),str(cpp),'-o',str(binary)],check=True,capture_output=True)
   subprocess.run([str(binary)],check=True,capture_output=True)
 def test_actual_storage_functions_with_fake_nvs(self):
  source=(ROOT/'main/driver/src/connect_wifi.cpp').read_text();start=source.index('namespace {\nstruct SavedNetwork');end=source.index('bool MaintenanceStaReady',start)
  harness=r'''
#include <cstdint>
#include <cstring>
#include <cassert>
#include <vector>
#include "saved_network_policy.hpp"
using esp_err_t=int;using nvs_handle_t=int;
constexpr int ESP_OK=0,ESP_ERR_NOT_FOUND=1,ESP_ERR_INVALID_ARG=2,NVS_READONLY=0,NVS_READWRITE=1;
std::vector<unsigned char> stored,pending;
int fail_open=0,fail_get=0,fail_set=0,fail_commit=0,sta_calls=0,sta_result=0,write_calls=0;
int nvs_open(const char* name,int,nvs_handle_t* h){assert(std::strcmp(name,"maint_net")==0);*h=1;return fail_open;}
void nvs_close(int){}
int nvs_get_blob(int,const char* key,void* data,std::size_t* size){assert(std::strcmp(key,"network")==0);if(fail_get||stored.empty())return 1;if(!data){*size=stored.size();return 0;}if(*size<stored.size())return 1;*size=stored.size();std::memcpy(data,stored.data(),stored.size());return 0;}
int nvs_set_blob(int,const char*,const void* data,std::size_t n){write_calls++;if(fail_set)return 1;stored.assign((const unsigned char*)data,(const unsigned char*)data+n);return 0;}
int nvs_commit(int){if(fail_commit)return 1;return 0;}
void mbedtls_platform_zeroize(void* p,std::size_t n){std::memset(p,0,n);}
int StartMaintenanceSta(const char* s,const char* p){sta_calls++;assert(std::strcmp(s,"synthetic-net")==0);assert(std::strcmp(p,"synthetic-password")==0);return sta_result;}
'''+source[start:end]+r'''
int main(){
 assert(!HasSavedMaintenanceNetwork());assert(StartSavedMaintenanceSta()==ESP_ERR_NOT_FOUND);assert(sta_calls==0);
 assert(SaveMaintenanceNetwork("synthetic-net","synthetic-password")==0);assert(HasSavedMaintenanceNetwork());assert(StartSavedMaintenanceSta()==0);assert(sta_calls==1);
 const auto good=stored;
 for(int* fault:{&fail_open,&fail_get}){*fault=1;assert(StartSavedMaintenanceSta()!=0);*fault=0;}
 stored[0]=2;assert(StartSavedMaintenanceSta()==ESP_ERR_NOT_FOUND);stored=good;
 std::memset(stored.data()+4,'x',33);assert(StartSavedMaintenanceSta()==ESP_ERR_NOT_FOUND);stored=good;
 stored.pop_back();assert(!HasSavedMaintenanceNetwork());assert(StartSavedMaintenanceSta()==ESP_ERR_NOT_FOUND);stored=good;
 for(int* fault:{&fail_open,&fail_set}){*fault=1;assert(SaveMaintenanceNetwork("synthetic-net","synthetic-password")!=0);assert(stored==good);*fault=0;}
 fail_commit=1;assert(SaveMaintenanceNetwork("replacement-net","replacement-password")!=0);assert(stored!=good);assert(HasSavedMaintenanceNetwork());fail_commit=0;stored=good;
 int before=write_calls;assert(SaveMaintenanceNetwork("synthetic-net","short")==ESP_ERR_INVALID_ARG);assert(write_calls==before);
 sta_result=9;assert(StartSavedMaintenanceSta()==9);assert(stored==good);
}
'''
  with tempfile.TemporaryDirectory() as directory:
   cpp=Path(directory)/'test.cpp';binary=Path(directory)/'test';cpp.write_text(harness)
   subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(ROOT/'main/ota'),str(cpp),'-o',str(binary)],check=True)
   subprocess.run([str(binary)],check=True,capture_output=True)
if __name__=='__main__':unittest.main()
