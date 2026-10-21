// Comet behaviour uses render frames, including packetized provider positions.
{
    GlyphEffectFrame f;f.width=800;f.height=450;f.clip={10,10,790,440};f.style=1;f.stream=1;f.advancing=true;
    GlyphEmitter e;e.corners={D2D1_POINT_2F{300,170},{380,170},{380,250},{300,250}};e.color={.6f,.8f,1,1};e.age=1;e.active=1;e.progress=.4f;e.seed=12;
    f.Add(e);LyricFairies swarm;float slow=1000,fast=0,closest=1000;size_t history=0;
    for(int n=0;n<=480;++n){f.seconds=n/60.f;f.presentationMs=10000+std::lround(n*1000./60);
        const auto before=swarm.agents[0].position;swarm.Step(f);const auto p=swarm.agents[0].position;
        expect(swarm.count==2,"Ordinary themes need two comets");
        expect(std::isfinite(p.x)&&std::isfinite(p.y),"Comet position became nonfinite");
        if(n){const float step=std::hypot(p.x-before.x,p.y-before.y);if(step>=7)std::cout<<"Large step frame="<<n<<" distance="<<step<<" intent="<<int(swarm.intent)<<" speed="<<std::hypot(swarm.agents[0].velocity.x,swarm.agents[0].velocity.y)<<"\n";expect(step<7,"Comet teleported within a normal frame");}
        if(n>120){const float speed=std::hypot(swarm.agents[0].velocity.x,swarm.agents[0].velocity.y);slow=std::min(slow,speed);fast=std::max(fast,speed);}
        const auto b=swarm.agents[1].position;closest=std::min(closest,std::hypot(p.x-b.x,p.y-b.y));
        history=std::max(history,swarm.agents[0].count);expect(history<=FairySamples,"Unbounded flight history");
    }
    std::cout<<"Comets speed="<<slow<<".."<<fast<<" separation="<<closest<<" history="<<history<<"\n";
    expect(fast>60&&slow<fast*.5f,"Flight lacks changing speed");expect(closest>11,"Comets merge into one orb");
    expect(history==FairySamples,"No sustained comet history");
    const auto pausedAgents=swarm.agents;const auto time=swarm.Time();f.paused=true;
    for(int i=0;i<15;++i){f.presentationMs+=16;swarm.Step(f);}
    expect(std::memcmp(pausedAgents.data(),swarm.agents.data(),sizeof(pausedAgents))==0&&swarm.Time()==time,"Explicit pause changed flight/history");
    f.paused=false;const auto previous=swarm.agents[0].position;
    for(auto& p:f.emitters[0].corners){p.x+=200;p.y+=60;}
    f.presentationMs+=16;f.seconds+=.016f;swarm.Step(f);
    expect(std::hypot(swarm.agents[0].position.x-previous.x,swarm.agents[0].position.y-previous.y)<10,"Next line teleports comet");
    for(int i=0;i<40;++i){f.seconds+=.016f;f.presentationMs+=16;swarm.Step(f);}
    expect(swarm.agents[0].position.x>450,"Comet fails to catch next line");
    f.seconds-=.025f;f.presentationMs+=16;swarm.Step(f);expect(swarm.agents[0].count>40,"Small provider correction resets tail");
    f.seconds=1.25f;f.presentationMs+=16;swarm.Step(f);expect(swarm.agents[0].count==1,"Backward seek retains old trail");
    f.stream=2;swarm.Step(f);expect(swarm.agents[0].count==1,"Track change retains old trail");
    f.presentationMs+=800;swarm.Step(f);expect(swarm.agents[0].count==1,"Long render gap joins stale wake");
    f.quality=3;swarm.Step(f);expect(swarm.count==2,"Low quality removes the interaction partner");
    f.reduced=true;swarm.Step(f);expect(swarm.count==0,"Reduced motion retains comets");
    f.reduced=false;f.count=0;swarm.Step(f);expect(swarm.count==0,"Cleared lyrics retain comets");
    // All provider packets use a 200 ms cadence, while rendering at 60 Hz.
    auto packet=f;packet.Add(e);packet.quality=0;packet.stream=3;packet.advancing=false;packet.sampled=true;
    LyricFairies sampled;int moved=0,frames=0;double last=0;
    for(int i=0;i<=240;++i){packet.presentationMs=20000+std::lround(i*1000./60);packet.seconds=std::floor(i/12.f)*.2f;
        const auto before=sampled.agents[0].position;sampled.Step(packet);
        if(i>20){++frames;if(std::hypot(sampled.agents[0].position.x-before.x,sampled.agents[0].position.y-before.y)>.001)++moved;
            expect(sampled.Time()>last,"Packetized playback freezes display-frame motion");}
        last=sampled.Time();}
    expect(moved==frames,"Comet only moves when a media packet arrives");
    const auto expireAt=packet.presentationMs;for(int i=1;i<=35;++i){packet.presentationMs=expireAt+i*16;sampled.Step(packet);}
    const auto stopped=sampled.Time();for(int i=0;i<8;++i){packet.presentationMs+=16;sampled.Step(packet);}
    expect(sampled.Time()==stopped,"Stale unknown-state media keeps flying indefinitely");
    // A missing projection during a short phrase transition cannot reset flight.
    packet.advancing=true;packet.sampled=false;packet.allowGap=true;packet.presentationMs+=16;sampled.Step(packet);
    const auto beforeGap=sampled.agents[0].position;packet.count=0;
    for(int i=0;i<6;++i){packet.presentationMs+=16;sampled.Step(packet);}
    expect(sampled.count==2&&sampled.agents[0].count>1,"Short phrase gap destroys tail");
    packet.Add(e);packet.presentationMs+=16;sampled.Step(packet);expect(sampled.agents[0].count>1,"Returning glyph respawns comet");
    packet.count=0;for(int i=0;i<20;++i){packet.presentationMs+=16;sampled.Step(packet);}expect(sampled.count==0,"Missing lyrics retain stale tail beyond bound");
    // Target follows singing progress, not the brightness/entry envelope.
    auto target=f;target.count=0;target.quality=0;target.advancing=true;target.presentationMs=0;target.seconds=0;
    auto old=e;old.progress=1;old.active=1;target.Add(old);auto current=e;current.progress=.3f;current.active=.01f;
    for(auto& p:current.corners)p.x+=220;target.Add(current);LyricFairies follows;follows.Step(target);
    expect(follows.agents[0].position.x>450,"Bright completed glyph overrides current sung glyph");
    auto run=[&](int fps){LyricFairies s;auto frame=f;frame.count=0;frame.Add(e);frame.quality=0;frame.style=9;frame.stream=7;
        for(int i=0;i<=fps*4;++i){frame.seconds=float(i)/fps;frame.presentationMs=std::lround(i*1000./fps);s.Step(frame);}return s;};
    const auto a=run(30),b=run(60),c=run(120),d=run(60);
    for(size_t i=0;i<a.count;++i){
        expect(std::hypot(a.agents[i].position.x-b.agents[i].position.x,a.agents[i].position.y-b.agents[i].position.y)<.1,"30/60 Hz changes flight path");
        expect(std::hypot(b.agents[i].position.x-c.agents[i].position.x,b.agents[i].position.y-c.agents[i].position.y)<.1,"60/120 Hz changes flight path");
        expect(b.agents[i].position.x==d.agents[i].position.x&&b.agents[i].position.y==d.agents[i].position.y,"Reset is not deterministic");
        const auto& h=b.agents[i];for(size_t j=1;j<h.count;++j)expect(std::abs(h.history[j].time-h.history[j-1].time-1./60)<.00001,"Tail samples depend on render cadence");
    }
}
// Visit actual different glyphs, physically exchange their destinations, and
// respond to the partner's recorded path rather than two unrelated formulas.
{
    GlyphEffectFrame f;f.width=1000;f.height=600;f.clip={20,20,980,580};f.style=9;f.stream=77;f.phrase=1;f.advancing=true;
    for(int i=0;i<4;++i){GlyphEmitter e;const float x=250.f+i*110;
        e.corners={D2D1_POINT_2F{x,250},{x+80,250},{x+80,330},{x,330}};e.color={.4f,.8f,1,1};e.age=2;e.seed=41.f+i*173;e.progress=i<1?1.f:i==1?.4f:0.f;f.Add(e);}
    LyricFairies s;std::array<bool,4> visited{};int physicalExchanges=0;bool testedPursuit=false;float maxStep=0;
    auto centreFor=[&](uint32_t key){for(size_t k=0;k<f.count;++k){const auto& e=f.emitters[k];if(key==uint32_t(e.seed)*64+uint32_t(k)+1)return D2D1_POINT_2F{e.corners[0].x+40,e.corners[0].y+40};}return D2D1_POINT_2F{-10000,-10000};};
    for(int n=0;n<=1200;++n){f.presentationMs=std::lround(n*1000./60);f.seconds=n/60.f;
        const auto prior=s.agents;const auto intent=s.intent;s.Step(f);
        if(n)for(size_t i=0;i<s.count;++i)maxStep=std::max(maxStep,std::hypot(s.agents[i].position.x-prior[i].position.x,s.agents[i].position.y-prior[i].position.y));
        for(size_t i=0;i<s.count;++i)for(size_t k=0;k<f.count;++k){const auto& e=f.emitters[k];const auto p=s.agents[i].position;
            if(std::abs(p.x-e.corners[0].x-40)<45&&std::abs(p.y-e.corners[0].y-40)<90)visited[k]=true;}
        if(n&&intent==CometIntent::Exchange&&s.intent!=intent&&prior[0].anchor!=prior[1].anchor){
            const auto a=centreFor(prior[0].anchor),b=centreFor(prior[1].anchor);
            if(std::hypot(a.x-prior[0].position.x,a.y-prior[0].position.y)<100&&std::hypot(b.x-prior[1].position.x,b.y-prior[1].position.y)<100)++physicalExchanges;
        }
        if(!testedPursuit&&s.intent==CometIntent::Chase&&s.agents[s.leader].count>30){
            auto original=s,perturbed=s;for(size_t k=0;k<perturbed.agents[s.leader].count;++k)perturbed.agents[s.leader].history[k].x+=30;
            auto next=f;next.presentationMs+=16;next.seconds+=.016f;original.Step(next);perturbed.Step(next);
            const size_t follower=(s.leader+1)%s.count;
            expect(std::abs(original.agents[follower].velocity.x-perturbed.agents[follower].velocity.x)>.5f,"Pursuit ignores the partner's actual wake");testedPursuit=true;
        }
    }
    const size_t visitedCount=std::count(visited.begin(),visited.end(),true);
    std::cout<<"Interaction visited="<<visitedCount<<" physical exchanges="<<physicalExchanges<<" chase phases="<<s.chases<<" maxStep="<<maxStep<<"\n";
    expect(visitedCount>=3,"Comets do not travel between distinct glyphs");expect(physicalExchanges>=2,"Exchange intent never reaches exchanged glyphs");
    expect(testedPursuit,"No interactive pursuit occurs");expect(maxStep<13,"Interactive route teleports between glyphs");
    expect(s.agents[0].size>=8.5f,"Comet head was not enlarged for an 80 DIP glyph");
    // Same song, new phrase: no history reset and a deliberate lead/follow handoff.
    const auto positions=s.agents;const auto oldLeader=s.leader;const auto oldRelays=s.relays;
    for(size_t i=0;i<f.count;++i)for(auto& p:f.emitters[i].corners){p.x-=100;p.y+=150;}
    ++f.phrase;f.presentationMs+=16;f.seconds+=.016f;s.Step(f);
    expect(s.relays==oldRelays+1&&s.leader!=oldLeader,"New phrase does not hand off the lead");
    expect(s.agents[0].count>60&&s.agents[1].count>60,"Phrase relay discards continuous trails");
    for(size_t i=0;i<s.count;++i)expect(std::hypot(s.agents[i].position.x-positions[i].position.x,s.agents[i].position.y-positions[i].position.y)<15,"Phrase relay teleports");
    for(int n=0;n<65;++n){f.presentationMs+=16;f.seconds+=.016f;s.Step(f);}
    expect(s.agents[0].position.y>340&&s.agents[1].position.y>340,"Relay does not reach the new phrase");
    // The light budget must still contain a pair at the lowest quality.
    f.quality=3;f.presentationMs+=16;s.Step(f);expect(s.count==2,"Low quality removes interaction");
    // Fast successive syllables and short phrases should keep finite, bounded
    // flight instead of restarting or jumping to each new word.
    for(int n=0;n<240;++n){f.presentationMs+=16;f.seconds+=.016f;if(n%35==0)++f.phrase;
        for(size_t i=0;i<f.count;++i)f.emitters[i].progress=i<size_t((n/6)%4)?1.f:i==size_t((n/6)%4)?.3f:0.f;
        const auto before=s.agents;s.Step(f);
        for(size_t i=0;i<s.count;++i){const auto p=s.agents[i].position;expect(std::isfinite(p.x)&&std::isfinite(p.y)&&p.x>=20&&p.x<=980&&p.y>=20&&p.y<=580,"Fast relay escapes stage");
            expect(std::hypot(p.x-before[i].position.x,p.y-before[i].position.y)<20,"Fast relay creates a visible jump");}
    }
}
