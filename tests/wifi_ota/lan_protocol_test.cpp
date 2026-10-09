#include "lan_window_protocol.hpp"
#include "saved_network_policy.hpp"
#include "maintenance_lifetime.hpp"
#include <cassert>
#include <string>
using namespace satori::ota;
int main() {
    std::vector<std::uint8_t> command(12,0);command[0]=1;command[1]=1;WindowWriteLe32(command.data()+2,42);
    const std::string ssid="test-network",password="test-password";command[10]=ssid.size();command[11]=password.size();
    command.insert(command.end(),ssid.begin(),ssid.end());command.insert(command.end(),password.begin(),password.end());
    LanCommand decoded{};assert(DecodeLanCommand(command.data(),command.size(),decoded));
    assert(decoded.window.request_id==42&&std::string(decoded.ssid.data())==ssid&&std::string(decoded.password.data())==password);
    auto bad=command;bad[11]=7;assert(!DecodeLanCommand(bad.data(),bad.size(),decoded));
    bad=command;bad[12]=0;assert(!DecodeLanCommand(bad.data(),bad.size(),decoded));
    bad=command;bad.back()=0xff;assert(!DecodeLanCommand(bad.data(),bad.size(),decoded));
    bad=command;bad[0]=2;assert(!DecodeLanCommand(bad.data(),bad.size(),decoded));
    bad=command;WindowWriteLe32(bad.data()+6,1);assert(!DecodeLanCommand(bad.data(),bad.size(),decoded));
    bad.assign(12,0);bad[0]=1;bad[1]=2;WindowWriteLe32(bad.data()+2,43);WindowWriteLe32(bad.data()+6,77);
    assert(DecodeLanCommand(bad.data(),bad.size(),decoded));assert(decoded.password[0]==0&&decoded.ssid[0]==0);
    bad.push_back(0);assert(!DecodeLanCommand(bad.data(),bad.size(),decoded));
    const std::string token(32,'a');auto status=EncodeLanStatus({WindowState::Open,WindowResult::Ok,42,77,99000},0,{192,168,1,80},token);
    assert(status.size()==56&&status[20]==32&&status[16]==192&&WindowReadLe32(status.data()+8)==77);
    assert(std::string(status.begin(),status.end()).find(password)==std::string::npos);
    assert(EncodeLanStatus({WindowState::Closed},0,{},token).empty());
    assert(EncodeLanStatus({WindowState::Failed},3,{},{}).size()==24);
    assert(EncodeLanStatus({WindowState::Open},0,{},std::string(32,'z')).empty());
    assert(WindowBearerMatches(token,token));assert(!WindowBearerMatches(token,std::string(32,'b')));assert(!WindowBearerMatches(token,""));
    std::vector<std::uint8_t> saved(12,0);saved[0]=2;saved[1]=1;WindowWriteLe32(saved.data()+2,44);
    assert(DecodeLanCommand(saved.data(),saved.size(),decoded));assert(decoded.use_saved_network&&decoded.ssid[0]==0&&decoded.password[0]==0);
    for(const auto offset:{1,6,10,11}) {auto invalid=saved;invalid[offset]=2;assert(!DecodeLanCommand(invalid.data(),invalid.size(),decoded));}
    auto invalid=saved;invalid.push_back(0);assert(!DecodeLanCommand(invalid.data(),invalid.size(),decoded));
    saved[0]=1;assert(!DecodeLanCommand(saved.data(),saved.size(),decoded)); // empty v1 never becomes saved reuse
    assert(EncodeLanStatus({WindowState::Failed},5,{},{}).size()==24);
    assert(SavedMaintenanceCredentialsValid("old-hotspot","synthetic-password"));
    assert(!SavedMaintenanceCredentialsValid("","synthetic-password"));
    assert(!SavedMaintenanceCredentialsValid("old-hotspot",""));
    assert(!SavedMaintenanceCredentialsValid(std::string(33,'s'),"synthetic-password"));
    assert(!SavedMaintenanceCredentialsValid("old-hotspot",std::string(64,'p')));
    assert(!SavedMaintenanceCredentialsValid("old-hotspot","synthetic\npassword"));
    assert(!SavedMaintenanceCredentialsValid(std::string("hot\0spot",8),"synthetic-password"));
    auto remember=command;remember[0]=3;
    assert(DecodeLanCommand(remember.data(),remember.size(),decoded));assert(decoded.remember_network&&!decoded.use_saved_network);
    saved[0]=3;assert(!DecodeLanCommand(saved.data(),saved.size(),decoded));
    assert(MaintenanceRemaining(599999,0,false,0)==1);
    assert(MaintenanceRemaining(600000,0,false,0)==0);
    assert(MaintenanceRemaining(610000,0,true,599000)==109000);
    assert(MaintenanceRemaining(719000,0,true,599000)==0);
    assert(MaintenanceRemaining(50,0xffffff00u,false,0)==600000-306);
    LanReplayGuard guard;std::array<std::uint8_t,32> digest{};
    assert(guard.Accept(42,digest));assert(guard.Accept(42,digest));digest[0]=1;assert(!guard.Accept(42,digest));
    WindowProtocol p;assert(!p.Admit({1,1,0},WindowState::Committed,0,true).execute);
    assert(!p.Admit({2,2,77},WindowState::Committed,77,true).execute); // other transport can't close active one
    assert(p.Admit({1,3,0},WindowState::Closed,77,true).execute);
    assert(!p.Admit({1,3,0},WindowState::Open,78,true).execute); // retry does not extend
}
