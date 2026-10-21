// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <array>
namespace stage {
enum class LyricMotion {Rift,Sonnet,Tempera,Lumiere,Classic,Cadenza,Partita,Fume,Tilt,Claddagh,Monet,Pendolo,Cappella,Diorama,Still,Mineradio};
inline bool UsesSyllableEntrance(LyricMotion motion,int particlePreset=0){
    return motion==LyricMotion::Rift||motion==LyricMotion::Sonnet||motion==LyricMotion::Fume||motion==LyricMotion::Tilt||motion==LyricMotion::Diorama||
        motion==LyricMotion::Tempera||motion==LyricMotion::Cadenza||motion==LyricMotion::Pendolo||motion==LyricMotion::Cappella||
        (motion==LyricMotion::Mineradio&&(particlePreset==3||particlePreset==8));
}
struct MotionStyle {LyricMotion kind;const wchar_t* name;const wchar_t* sample;unsigned background,panel,ink,muted,accent;};
inline constexpr int FirstFoliaStyle=7;
inline constexpr std::array<MotionStyle,14> FoliaStyles={{
    {LyricMotion::Sonnet,L"商籁",L"写成一首诗",0x101113,0x202226,0xf4f1ea,0x909497,0xe5c6a6},
    {LyricMotion::Tempera,L"凝彩",L"让色彩流动",0xf4f0e6,0xe2d7bf,0x181a20,0x777b76,0x627b74},
    {LyricMotion::Lumiere,L"绘光",L"追着光前行",0x080b10,0x171e27,0xfff3dc,0x9c9c9c,0xe7c281},
    {LyricMotion::Classic,L"流光",L"你是我的光",0x111820,0x1e2932,0xf2f5f4,0x91a5af,0x99d4d0},
    {LyricMotion::Cadenza,L"心象",L"心中的景象",0x15111a,0x27202f,0xf6f1f7,0xa594ae,0xdda6c9},
    {LyricMotion::Partita,L"云阶",L"沿着云向上",0x101d23,0x1d3036,0xf2f3ed,0x90a9a9,0xb0d8c7},
    {LyricMotion::Fume,L"浮名",L"文字随风远行",0x101215,0x1f2328,0xf6f2e9,0xa3a3a0,0xe5c49c},
    {LyricMotion::Tilt,L"倾诉",L"把心事说给你",0x151119,0x29202d,0xf5edf4,0xb19aaf,0xe8afcc},
    {LyricMotion::Claddagh,L"回环",L"回到你的身边",0x10171b,0x1d2b31,0xeef4f3,0x97afb6,0x9edbd7},
    {LyricMotion::Monet,L"莫奈",L"看见生活的颜色",0xeee8dc,0xdad8c8,0x30372f,0x849080,0x708c72},
    {LyricMotion::Pendolo,L"时计",L"时光缓缓转动",0x111714,0x263028,0xeeeada,0x9ba58f,0xc5c08a},
    {LyricMotion::Cappella,L"群唱",L"一起唱这首歌",0x171a21,0x2a303b,0xf1f4f6,0x9caabd,0xa8cbea},
    {LyricMotion::Diorama,L"镜台",L"镜中的远方",0x0b101a,0x182333,0xecf3fa,0x8095ad,0x8cc4f1},
    {LyricMotion::Still,L"静止",L"安静听这一句",0x101214,0x1e2225,0xf3f1e9,0x979b9d,0xd8d1be}
}};
inline constexpr bool IsFoliaStyle(int style){return style>=FirstFoliaStyle&&style<FirstFoliaStyle+static_cast<int>(FoliaStyles.size());}
inline constexpr int FirstMineradioStyle=21;
inline constexpr std::array<const wchar_t*,13> MineradioNames={L"专辑粒子",L"滚筒",nullptr,L"虚空",L"唱片",L"星河",nullptr,L"音域回响（原生重做）",L"空间浪潮（原生重做）",nullptr,nullptr,nullptr,nullptr};
// Persisted IDs are never reassigned when a built-in is removed.
inline constexpr std::array<int,28> AvailableBuiltInStyles={0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,24,25,26,28,29};
inline constexpr bool IsRemovedBuiltInStyle(int style){return style==23||style==27||(style>=30&&style<=33);}
inline constexpr int NormalizeBuiltInStyle(int style){return IsRemovedBuiltInStyle(style)?21:style<0||style>=34?0:style;}
inline constexpr int NormalizeMineradioPreset(int preset){const int style=NormalizeBuiltInStyle(21+preset);return style>=21?style-21:0;}
inline constexpr int ThemeChoiceIndex(int style){for(size_t i=0;i<AvailableBuiltInStyles.size();++i)if(AvailableBuiltInStyles[i]==NormalizeBuiltInStyle(style)&&style!=34)return static_cast<int>(i);return 28;}
inline constexpr int ThemeIdAtChoice(int choice){return choice>=0&&choice<28?AvailableBuiltInStyles[choice]:choice==28?34:-1;}
inline constexpr int MotionVariantCount(int style){return style==7||style==9?100:style==8?121:style==19?13:0;}
inline constexpr bool IsMineradioStyle(int style){return style>=FirstMineradioStyle&&style<FirstMineradioStyle+static_cast<int>(MineradioNames.size())&&!IsRemovedBuiltInStyle(style);}
inline constexpr bool IsGlyphMotionStyle(int style){return style==6||IsFoliaStyle(style)||IsMineradioStyle(style);}
inline constexpr LyricMotion MotionKind(int style){return IsMineradioStyle(style)?LyricMotion::Mineradio:IsFoliaStyle(style)?FoliaStyles[style-FirstFoliaStyle].kind:LyricMotion::Rift;}
inline constexpr const wchar_t* MotionName(int style){return IsMineradioStyle(style)?MineradioNames[style-FirstMineradioStyle]:IsFoliaStyle(style)?FoliaStyles[style-FirstFoliaStyle].name:L"裂光现场";}
}
