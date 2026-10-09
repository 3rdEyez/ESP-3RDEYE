"""No sockets: run IDF's actual admission/dispatch/close code with fake IO."""
import os,re,subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
IDF=Path(os.environ['IDF_PATH']) if os.environ.get('IDF_PATH') else None
def function(source,name):
 match=re.search(r'^(?:static )?(?:esp_err_t|int|void) '+re.escape(name)+r'\([^\n]*\)\s*\{',source,re.M)
 if not match:raise AssertionError(name)
 begin=source.index('{',match.start());depth=1;at=begin+1
 while depth:
  if source[at]=='{':depth+=1
  if source[at]=='}':depth-=1
  at+=1
 return source[match.start():at]
HARNESS=r'''
#include <cassert>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <algorithm>
#include <vector>
#include "ota_http_policy.hpp"
using namespace satori::ota;
using esp_err_t=int;using httpd_handle_t=void*;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NOT_FOUND 1
#define THREAD_STOPPING 1
#define HTTPD_TASK_FIND_LOWEST_LRU 1
#define HTTP_SERVER_EVENT_ON_CONNECTED 1
#define HTTPD_403_FORBIDDEN 403
#define HTTPD_RESP_USE_STRLEN -1
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define LOG_FMT(x) x
#define MAX(a,b) std::max(a,b)
static const int DEFAULT_KEEP_ALIVE_IDLE=5,DEFAULT_KEEP_ALIVE_INTERVAL=5,DEFAULT_KEEP_ALIVE_COUNT=3;
struct sock_db {int fd=-1;bool for_async_req=false,lru_socket=false;uint64_t lru_counter=0;void* handle=nullptr;};
struct Config {bool lru_purge_enable=SATORI_OTA_HTTP_LRU_PURGE,keep_alive_enable=false;int recv_wait_timeout=3,send_wait_timeout=3,keep_alive_idle=0,keep_alive_interval=0,keep_alive_count=0;};
struct httpd_data {Config config;int listen_fd=3,ctrl_fd=4;struct {int status=0;} hd_td;sock_db slot;};
struct enum_context_t {int task;uint64_t lru_counter;int fd;sock_db* session=nullptr;};
struct process_session_context_t {fd_set* fdset;httpd_data* hd;};
struct httpd_req_t {};
static bool handler_running=false,closed_header=false,sent=false,listen_requested=false;
static int closes=0,accepts=0,body_result=0,body_calls=0;static bool queue_fails=false;
static httpd_data* current=nullptr;static std::vector<std::pair<void(*)(void*),void*>> work;
static int httpd_process_session(sock_db*,void*);
static bool httpd_is_sess_available(httpd_data* h){return h->slot.fd<0;}
static int enum_function(sock_db* s,void* ctx){auto* c=static_cast<enum_context_t*>(ctx);c->session=s;return 1;}
static void httpd_sess_enum(httpd_data* h,int(*callback)(sock_db*,void*),void* ctx){if(h->slot.fd>=0)callback(&h->slot,ctx);}
static void httpd_sess_delete(httpd_data*,sock_db* s){assert(!handler_running);closes++;s->fd=-1;s->lru_counter=0;}
static esp_err_t httpd_queue_work(httpd_handle_t,void(*callback)(void*),void* arg){if(queue_fails)return ESP_FAIL;work.push_back({callback,arg});return ESP_OK;}
static int fake_accept(int,sockaddr*,socklen_t*){assert(!handler_running);accepts++;return 5;}
static int fake_setsockopt(int,int,int,const void*,socklen_t){return 0;}
static int fake_close(int){assert(!handler_running);return 0;}
static int httpd_sess_new(httpd_data* h,int fd){assert(h->slot.fd<0);h->slot.fd=fd;h->slot.handle=h;h->slot.lru_counter=1;return ESP_OK;}
static void esp_http_server_dispatch_event(int,const void*,size_t){}
static bool httpd_sess_pending(httpd_data*,sock_db*){return false;}
static void httpd_sess_set_descriptors(httpd_data* h,fd_set* f,int* high){if(h->slot.fd>=0)FD_SET(h->slot.fd,f);*high=5;}
static void httpd_sess_delete_invalid(httpd_data*){}
static void httpd_process_ctrl_msg(httpd_data*){if(!work.empty()){auto item=work.front();work.erase(work.begin());item.first(item.second);}}
static int fake_select(int,fd_set* f,fd_set*,fd_set*,timeval*){listen_requested=FD_ISSET(3,f);FD_ZERO(f);if(listen_requested)FD_SET(3,f);if(!work.empty())FD_SET(4,f);if(current->slot.fd>=0)FD_SET(current->slot.fd,f);return 1;}
static bool Alive(){return true;}
static bool OfficialUpdateLayoutReady(){return true;}
static int httpd_resp_set_hdr(httpd_req_t*,const char* name,const char* value){if(!strcmp(name,"Connection"))closed_header=!strcmp(value,"close");return ESP_OK;}
static int httpd_resp_set_type(httpd_req_t*,const char*){return ESP_OK;}
static int httpd_resp_send_err(httpd_req_t*,int,const char*){sent=true;return ESP_OK;}
static int httpd_resp_send(httpd_req_t*,const char* p,int){assert(strstr(p,"fetch('/v1/ota/image'"));assert(closed_header);sent=true;return ESP_OK;}
static int UploadBody(httpd_req_t*){handler_running=true;body_calls++;assert(closes==0);assert(accepts==0);handler_running=false;sent=true;return body_result;}
static int httpd_sess_process(httpd_data*,sock_db*){handler_running=true;assert(closes==0);assert(accepts==0);handler_running=false;return ESP_OK;}
#define accept fake_accept
#define setsockopt fake_setsockopt
#define close fake_close
#define select fake_select
'''
MAIN=r'''
int main(){
 static_assert(SATORI_OTA_HTTP_MAX_OPEN_SOCKETS==1);
 httpd_data h;current=&h;h.slot.fd=5;h.slot.handle=&h;h.slot.lru_counter=1;
 // Actual IDF loop must listen at capacity, then evict only after dispatch.
 assert(httpd_server(&h)==ESP_OK);assert(listen_requested);assert(accepts==0);
#if CONFIG_HTTPD_QUEUE_WORK_BLOCKING
 assert(closes==1);
#else
 assert(closes==0);assert(work.size()==1);
#endif
 assert(httpd_server(&h)==ESP_OK);assert(closes==1);assert(accepts==1);
 // Queue failure cannot admit over capacity or close the existing session.
#if !CONFIG_HTTPD_QUEUE_WORK_BLOCKING
 work.clear();closes=accepts=0;queue_fails=true;assert(httpd_server(&h)==ESP_OK);assert(closes==0&&accepts==0);queue_fails=false;
#endif
 // Production Page returns close only after fully sending the response.
 closes=accepts=0;httpd_req_t req;sent=closed_header=false;assert(Page(&req)==ESP_FAIL);assert(sent&&closed_header);
 // Production Upload wrapper applies the same close result to success/failure.
 for(int result:{ESP_OK,ESP_FAIL}){sent=closed_header=false;body_result=result;assert(Upload(&req)==ESP_FAIL);assert(sent&&closed_header);}
 assert(body_calls==2);return 0;
}
'''
@unittest.skipUnless(IDF, "Set IDF_PATH for native HTTP source regression")
class Tests(unittest.TestCase):
 def test_native_idf_single_thread_admission_and_production_response_handlers(self):
  main=(IDF/'components/esp_http_server/src/httpd_main.c').read_text();sess=(IDF/'components/esp_http_server/src/httpd_sess.c').read_text();service=(ROOT/'main/ota/wifi_ota_service.cpp').read_text()
  native='\n'.join(function(sess,n) for n in ('httpd_sess_close','httpd_sess_trigger_close_','httpd_sess_close_lru','httpd_sess_close_lru_direct'))
  native+='\n'+'\n'.join(function(main,n) for n in ('httpd_accept_conn','httpd_process_session','httpd_server'))
  native+='\n'+function(service,'Page')+'\n'+function(service,'Upload')
  self.assertNotIn('httpd_req_async_handler_begin',service)
  self.assertIn('http.max_open_sockets=SATORI_OTA_HTTP_MAX_OPEN_SOCKETS',service)
  self.assertIn('http.lru_purge_enable=SATORI_OTA_HTTP_LRU_PURGE',service)
  with tempfile.TemporaryDirectory() as d:
   source=Path(d)/'test.cpp';source.write_text(HARNESS+native+MAIN)
   for blocking in (0,1):
    binary=Path(d)/('native-'+str(blocking));run=subprocess.run(['c++','-std=c++20','-I'+str(ROOT/'main/ota'),'-DCONFIG_HTTPD_QUEUE_WORK_BLOCKING='+str(blocking),str(source),'-o',str(binary)],capture_output=True,text=True)
    self.assertEqual(run.returncode,0,run.stderr);subprocess.run([str(binary)],check=True)
if __name__=='__main__':unittest.main()
