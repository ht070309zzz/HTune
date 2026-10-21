// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <windows.h>
#include "third_party/json.hpp"
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace stage {
// RCDATA stays mapped in the executable. Index recipes without materializing
// every JSON number, array and map into a permanent heap object graph.
class MotionRecipes {
    mutable std::string_view bytes_;
    mutable std::vector<std::pair<size_t,size_t>> entries_;
    int resource_;
    std::string key_;
    void Initialize()const{
        if(!entries_.empty())return;
        const auto r=FindResourceW(nullptr,MAKEINTRESOURCEW(resource_),RT_RCDATA);
        if(!r)throw std::runtime_error("Missing motion recipe resource");
        const auto size=SizeofResource(nullptr,r);
        const auto* data=static_cast<const char*>(LockResource(LoadResource(nullptr,r)));
        if(!data||!size||size>16*1024*1024)throw std::runtime_error("Invalid motion recipe resource");
        bytes_={data,size};
        const auto keyAt=bytes_.find(std::string("\"")+key_+"\"");
        if(keyAt==std::string_view::npos)throw std::runtime_error("Missing recipe array");
        const auto array=bytes_.find('[',keyAt);
        if(array==std::string_view::npos)throw std::runtime_error("Invalid recipe array");
        std::vector<std::pair<size_t,size_t>> entries;
        bool quoted=false,escape=false;int depth=0;size_t start=0;
        for(size_t i=array+1;i<bytes_.size();++i){const char c=bytes_[i];
            if(quoted){if(escape)escape=false;else if(c=='\\')escape=true;else if(c=='"')quoted=false;continue;}
            if(c=='"'){quoted=true;continue;}
            if(c=='{'||c=='['){if(depth++==0)start=i;}
            else if(c=='}'||c==']'){
                if(depth==0){if(c==']'&&!entries.empty()){entries_=std::move(entries);return;}break;}
                if(--depth==0)entries.emplace_back(start,i+1);
            }
        }
        throw std::runtime_error("Incomplete recipe array");
    }
public:
    MotionRecipes(int resource,const char* key):resource_(resource),key_(key){}
    size_t Size()const{Initialize();return entries_.size();}
    nlohmann::json Load(size_t index)const{
        Initialize();
        const auto [begin,end]=entries_.at(index%entries_.size());
        return nlohmann::json::parse(bytes_.data()+begin,bytes_.data()+end);
    }
};
}
