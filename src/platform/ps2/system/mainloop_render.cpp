/* AURORA_V13_UNIFIED_GBC_AUDIO_32X_FRAMESKIP_20260910 */
/* mainloop_render.cpp
 *
 * Hosts MainLoopRender() and the file-static state it needs
 * (_uVblankCycle and the screen-size #defines used to clear / blit the
 * SNES output texture).
 *
 * Extracted from mainloop.cpp during the Batch 3 split. No logic,
 * literal, or attribute change.
 */

#include <stdio.h>
#include <string.h> /* AURORA_TSUKURU_8M_GBA_LOAD_AUDIO_REDERR_V1_20260913_RED_ERRORS */

#include "mainloop_debug.h"
#include "mainloop_shared.h"
#if AURORA_C4_PROFILER
#include "platform/ps2/system/aurora_c4_profiler.h"
#endif
#include "mainloop_ui.h"
#include "mainloop_menu.h" /* AURORA_FINAL_V1_5_RENDER_MENU_API_INCLUDE_20260901 */
#include "mainloop_bgm.h"
#include "mainloop_safe_frameskip.h" /* AURORA_SAFE_FRAMESKIP_GG_ZOOM_V2_2 */
#include "platform/ps2/system/aurora_snes_cost_profiler.h" /* AURORA_SNES_COST_PROFILER_V1_20260920 */
#if AURORA_FRONTEND_PROFILER
#include "platform/ps2/system/aurora_frontend_profiler.h"
#endif
#include "sega/picodrive/picodrive_bridge.h"
/* AURORA_ALLCORES_PERF_V5_20260824 */
#include "nes/quicknes/quicknes_bridge.h"
/* AURORA_FCEUMM_FDS_PERF_DIRECT_T8_V3_20260827 */
#include "nes/fceumm/fceumm_fds_bridge.h"
#include "pce/beetle/pce_bridge.h"
#include "gb/system/gambattesystem.h" /* AURORA_GAMBATTE_SQUARE_ASPECT_V13_20260911 */
#include "gba/system/gpspsystem.h" /* AURORA_GPSP_GBA_V16_DIRECT_GS_CT16_20260912 */

/* AURORA_PS2_PERF_V4_20260824 */
#include "types.h"
#include "console.h"
#include "snes.h"
#include "rendersurface.h"
#include "texture.h"
#include "font.h"
#include "poly.h"
#include "prof.h"
#include "snstate.h"
#include "snppublend_gs.h"
#include "common/debug/dbgterm.h"

/* AURORA_PCE_RELEASE_CLEANUP_FINAL_V15_20260830
 * Hidden release diagnostics. Keep the probes available for future
 * PCE/QuickNES investigations without drawing anything by default. */
#ifndef AURORA_RUNTIME_DIAG_OVERLAY
#define AURORA_RUNTIME_DIAG_OVERLAY 0
#endif

#include "mainloop_iop.h"

extern "C" {
#include "hw.h"
#include "gs.h"
#include "gpfifo.h"
#include "gpprim.h"
#include "gskit_backend.h"
#include "audio.h"
};

extern "C" {
#include "mcsave_ee.h"
};


/* Same MAINLOOP_SCREENWIDTH / HEIGHT pair as mainloop_init.cpp. The
   render path uses these to size the output blit; the init path uses
   them to size the GS framebuffer. Three other historical layouts are
   kept commented out in mainloop_init.cpp for reference. */
#define MAINLOOP_SCREENWIDTH 256
#define MAINLOOP_SCREENHEIGHT 240


static Uint32 _uVblankCycle;

/* AURORA_NOINTRO_CRC_FALLBACK_V1_20261028: resolvedor para gates por jogo, fora de hot paths.
 * O CRC real nunca é modificado: saves, ZIP, firmware e estados permanecem
 * validados pela identidade original. Apenas o CRC passado aos gates pode
 * receber um alias via nome No-Intro exato, sem extensão. */
struct AuroraNoIntroAliasT { Uint8 system; Uint32 crc; const char *name; };
static const AuroraNoIntroAliasT s_AuroraNoIntroAliases[] = {
    { 1u, 0x07E01EF9u, "Super Mario World 2 - Yoshi's Island (Europe) (En,Fr,De) (Rev 1)" },
    { 1u, 0x0F66698Bu, "Super Mario - Yoshi Island (Japan) (Rev 1)" },
    { 1u, 0x11A6E64Bu, "Hong Kong '97 (Japan) (En,Ja,Zh-Hant) (Unl)" },
    { 1u, 0x125A0C22u, "Dark Law - Meaning of Death (Japan)" },
    { 1u, 0x12F0A699u, "Tactics Ogre - Let Us Cling Together (Japan) (Rev 2) (NP)" },
    { 1u, 0x17657DB6u, "Donkey Kong Country (Europe) (En,Fr,De) (Rev 1)" },
    { 1u, 0x19369514u, "Top Racer (World) (Piko Interactive)" },
    { 1u, 0x19BDCB19u, "Derby Stallion 96 (Japan)" },
    { 1u, 0x266C44EDu, "Pilotwings (USA)" },
    { 1u, 0x271E1D07u, "Tactics Ogre - Let Us Cling Together (Japan) (Rev 1)" },
    { 1u, 0x2AA7777Bu, "Ardy Lightfoot (Japan)" },
    { 1u, 0x2B88BEE8u, "Top Gear 2 (USA)" },
    { 1u, 0x2E8034ABu, "Addams Family, The (USA)" },
    { 1u, 0x31DAEBB2u, "Top Racer 2 (Japan) (En)" },
    { 1u, 0x32E29079u, "Derby Stallion II (Japan)" },
    { 1u, 0x343E0DFBu, "Super Mario - Yoshi Island (Japan)" },
    { 1u, 0x36768B55u, "Derby Stallion III (Japan) (Rev 3)" },
    { 1u, 0x3AA01DBDu, "Lost Vikings 2 (USA)" },
    { 1u, 0x3ADEF543u, "Super Donkey Kong (Japan) (Rev 1)" },
    { 1u, 0x3C49A285u, "Lost Vikings II, The - Norse by Norsewest (Europe) (En,Fr,De)" },
    { 1u, 0x3EAA5697u, "Donkey Kong Country (USA) (Rev 1)" },
    { 1u, 0x493FDB13u, "Top Gear 3000 (Europe)" },
    { 1u, 0x4CD45939u, "Daisenryaku Expert WWII - War in Europe (Japan)" },
    { 1u, 0x4EE705ABu, "Lost Vikings, The (Germany)" },
    { 1u, 0x50F2D1BCu, "Super Donkey Kong (Japan)" },
    { 1u, 0x50FEF979u, "Viking no Daimeiwaku (Japan)" },
    { 1u, 0x51A5F489u, "Ardy Lightfoot (USA)" },
    { 1u, 0x525FFB26u, "Derby Stallion 98 (Japan) (NP)" },
    { 1u, 0x52ADA404u, "Sunset Riders (USA)" },
    { 1u, 0x531463E1u, "Top Gear 2 (Europe)" },
    { 1u, 0x5441F25Bu, "Sonic Blast Man (Europe)" },
    { 1u, 0x547CF7B9u, "Wizardry V - Saika no Chuushin (Japan) (En,Ja)" },
    { 1u, 0x56410E5Eu, "Super Mario Kart (Europe)" },
    { 1u, 0x5EBF7246u, "Sound Novel Tsukuru (Japan)" },
    { 1u, 0x5F3E5279u, "Derby Stallion III (Japan) (Rev 2)" },
    { 1u, 0x64EDFC5Du, "Sunset Riders (Europe)" },
    { 1u, 0x66989491u, "Lost Vikings, The (Europe)" },
    { 1u, 0x675B6382u, "RPG Tsukuru 2 (Japan)" },
    { 1u, 0x6838BE08u, "Lost Vikings, The (USA)" },
    { 1u, 0x6E8A1081u, "Lost Vikings, The (Spain)" },
    { 1u, 0x75EEEB3Fu, "Derby Stallion III (Japan)" },
    { 1u, 0x762AF827u, "Donkey Kong Country (USA) (Rev 2)" },
    { 1u, 0x77871727u, "Pilotwings (Japan)" },
    { 1u, 0x7A33E836u, "Donkey Kong Country (Europe) (En,Fr,De)" },
    { 1u, 0x7E97AE6Au, "Derby Stallion III (Japan) (Rev 1)" },
    { 1u, 0x82497F19u, "Addams Family, The (Europe) (En,Fr,De)" },
    { 1u, 0x857980B2u, "Super Mario World 2 - Yoshi's Island (Europe) (En,Fr,De)" },
    { 1u, 0x8886396Eu, "Sonic Blast Man (USA)" },
    { 1u, 0x94093298u, "Lost Vikings, The (France) (En,Fr)" },
    { 1u, 0xA20BE998u, "Top Gear 3000 (USA)" },
    { 1u, 0xAF415C24u, "Ganpuru - Gunman's Proof (Japan)" },
    { 1u, 0xB0150052u, "Top Gear (Europe)" },
    { 1u, 0xB17E95D4u, "Mini Yonku Shining Scorpion - Let's & Go!! (Japan)" },
    { 1u, 0xB9B9DF06u, "Planet's Champ TG 3000, The (Japan)" },
    { 1u, 0xBC1A265Fu, "Kakinoki Shougi (Japan)" },
    { 1u, 0xBE523800u, "Sonic Blast Man (Japan)" },
    { 1u, 0xC17ECBA5u, "Wizardry VI - Kindan no Mahitsu (Japan)" },
    { 1u, 0xC49D28A4u, "Magical Pop'n (Japan)" },
    { 1u, 0xC6A95816u, "Hong Kong '97 (Japan) (En,Ja,Zh-Hant) (CM) (Unl)" },
    { 1u, 0xC79C123Fu, "Ongaku Tsukuru Kanadeeru (Japan)" },
    { 1u, 0xC8002453u, "Super Mario Kart (Japan)" },
    { 1u, 0xC946DCA0u, "Donkey Kong Country (USA)" },
    { 1u, 0xCAF432AAu, "Donkey Kong Country (USA, Europe) (Rev 2) (Virtual Console, Classic Mini, Switch Online)" },
    { 1u, 0xCB0653D0u, "Speedy Gonzales - Los Gatos Bandidos (USA) (Rev 1)" },
    { 1u, 0xCD80351Cu, "Addams Family, The (Japan)" },
    { 1u, 0xCD80DB86u, "Super Mario Kart (USA)" },
    { 1u, 0xCF98DDAAu, "Super Mario World 2 - Yoshi's Island (USA) (Rev 1)" },
    { 1u, 0xD0DE3012u, "Speedy Gonzales in Los Gatos Bandidos (Europe) (Proto) (No. 04)" },
    { 1u, 0xD138F224u, "Super Mario World 2 - Yoshi's Island (USA)" },
    { 1u, 0xD239398Fu, "Ardy Lightfoot (Europe)" },
    { 1u, 0xD34C49B7u, "Top Gear (USA)" },
    { 1u, 0xDAF285A5u, "Tactics Ogre - Let Us Cling Together (Japan)" },
    { 1u, 0xDEF45776u, "Pilotwings (Europe)" },
    { 1u, 0xE20870EEu, "RPG Tsukuru - Super Dante (Japan)" },
    { 1u, 0xE2DBAD76u, "Speedy Gonzales - Los Gatos Bandidos (USA)" },
    { 1u, 0xE5A57B12u, "Top Racer (Japan) (En)" },
    { 1u, 0xECA2C2D1u, "Super Donkey Kong (Japan) (Rev 1) (Virtual Console, Classic Mini, Switch Online)" },
    { 1u, 0xEFE9E087u, "Speedy Gonzales in Los Gatos Bandidos (Europe) (Proto) (No. 14)" },
    { 1u, 0xF1063FADu, "Super Mario - Yoshi Island (Japan) (Rev 2)" },
    { 1u, 0xF1110FADu, "Down the World - Mervil's Ambition (Japan)" },
    { 2u, 0x0AFB395Eu, "Gun Sight (Japan)" },
    { 2u, 0x143DF524u, "Strike Wolf (Asia) (Unl)" },
    { 2u, 0x149C0EC3u, "Famicom Shougi - Ryuuousen (Japan)" },
    { 2u, 0x1BFE42ABu, "Mad City (Japan) (Virtual Console)" },
    { 2u, 0x1CA9C322u, "Liesha Zhuluoji - Blood of Jurassic (China) (En) (Unl)" },
    { 2u, 0x1CAE8DEFu, "Asder - 20-in-1 (USA) (Unl)" },
    { 2u, 0x1F6660E6u, "Barker Bill's Trick Shooting (Europe)" },
    { 2u, 0x21DD2174u, "Haja no Fuuin (Japan)" },
    { 2u, 0x231BC76Eu, "Chiller (Australia) (Unl)" },
    { 2u, 0x23D17F5Eu, "Lone Ranger, The (USA)" },
    { 2u, 0x24598791u, "Duck Hunt (World)" },
    { 2u, 0x2A3CA509u, "Downtown - Nekketsu Monogatari (Japan)" },
    { 2u, 0x2A6559A1u, "Operation Wolf (Japan)" },
    { 2u, 0x3488A174u, "Duck Hunt (Asia) (En) (Pirate)" },
    { 2u, 0x39A18397u, "Downtown - Nekketsu Monogatari (Japan) (Virtual Console)" },
    { 2u, 0x3BBFF3A6u, "Best Play Pro Yakyuu (Japan)" },
    { 2u, 0x3E58A87Eu, "Freedom Force (USA)" },
    { 2u, 0x4318A2F8u, "Barker Bill's Trick Shooting (USA)" },
    { 2u, 0x47C2020Bu, "Hydlide 3 - Yami kara no Houmonsha (Japan)" },
    { 2u, 0x497C6A69u, "Super Russian Roulette (World) (Aftermarket) (Unl)" },
    { 2u, 0x498187B6u, "Wizardry - Proving Grounds of the Mad Overlord (Japan)" },
    { 2u, 0x4D68CFB1u, "Crime Busters (Brazil) (En) (Unl)" },
    { 2u, 0x4E959173u, "Gotcha! - The Sport! (USA)" },
    { 2u, 0x5112DC21u, "Wild Gunman (World) (Rev 1)" },
    { 2u, 0x5529431Fu, "Operation Wolf (Europe)" },
    { 2u, 0x55397DB3u, "Wizardry II - Llylgamyn no Isan (Japan)" },
    { 2u, 0x5C123EF7u, "Wizardry III - Diamond no Kishi (Japan)" },
    { 2u, 0x5D4574E0u, "Hogan's Alley (Asia) (En) (Pirate)" },
    { 2u, 0x5EE6008Eu, "Mechanized Attack (USA)" },
    { 2u, 0x61061352u, "Master Shooter (Taiwan) (En) (Unl)" },
    { 2u, 0x61A852EAu, "Battle Stadium - Senbatsu Pro Yakyuu (Japan)" },
    { 2u, 0x64594DA3u, "Russian Roulette (World) (Aftermarket) (Unl)" },
    { 2u, 0x67751094u, "Adventures of Bayou Billy, The (USA)" },
    { 2u, 0x70DF0D3Du, "5-in-1 (1993) (Asia) (En) (Unl)" },
    { 2u, 0x73FB55ACu, "Lightgun Game 2 in 1 - Cosmocop + Cyber Monster (Taiwan) (En) (Unl)" },
    { 2u, 0x74BEA652u, "3-in-1 Supergun (Asia) (En) (Unl)" },
    { 2u, 0x78B657ACu, "Armadillo (Japan)" },
    { 2u, 0x790B295Bu, "To the Earth (Europe)" },
    { 2u, 0x7C4EBDACu, "Super 4-in-1 - Fantasy Gun (Asia) (En) (Unl)" },
    { 2u, 0x7FA2CC55u, "Castle Excellent (Japan) (En)" },
    { 2u, 0x803B9979u, "J.League Fighting Soccer - The King of Ace Strikers (Japan)" },
    { 2u, 0x8373021Eu, "Adventures of Bayou Billy, The (USA) (Virtual Console)" },
    { 2u, 0x83EAF3B1u, "Best Keiba - Derby Stallion (Japan) (Rev 1)" },
    { 2u, 0x851EB9BEu, "Shooting Range (USA)" },
    { 2u, 0x8C4D59D6u, "Derby Stallion - Zenkoku Ban (Japan)" },
    { 2u, 0x93216279u, "Rumble Station - 15 in 1 (USA) (Unl)" },
    { 2u, 0x974E8840u, "Best Play Pro Yakyuu '90 (Japan)" },
    { 2u, 0xA0FBF02Eu, "Lethal Weapon (Taiwan) (En) (Unl)" },
    { 2u, 0xA1A33B85u, "Best Keiba - Derby Stallion (Japan)" },
    { 2u, 0xB0480AE9u, "Laser Invasion (USA)" },
    { 2u, 0xB133CFA7u, "Baby Boomer (Korea) (En) (Unl)" },
    { 2u, 0xB79F2651u, "Chiller (USA) (Unl)" },
    { 2u, 0xB811C054u, "Fleet Commander (Japan)" },
    { 2u, 0xB8747ABFu, "Best Play Pro Yakyuu Special (Japan)" },
    { 2u, 0xB8B9ACA3u, "Wild Gunman (Japan, USA) (En)" },
    { 2u, 0xB9AB06AAu, "Money Game II, The - Kabutochou no Kiseki (Japan)" },
    { 2u, 0xBBE40DC4u, "Baby Boomer (USA) (Unl)" },
    { 2u, 0xBC9BFFCBu, "Adventures of Bayou Billy, The (Europe)" },
    { 2u, 0xBCFDD7DEu, "Falling Tiles (World) (v1.1) (Aftermarket) (Unl)" },
    { 2u, 0xBEB8AB01u, "Gumshoe (USA, Europe)" },
    { 2u, 0xC22BC87Bu, "Seiryaku Simulation - Inbou no Wakusei - Shancara (Japan)" },
    { 2u, 0xC2EF3422u, "Best Play Pro Yakyuu II (Japan)" },
    { 2u, 0xC3DE7C69u, "Best Play Pro Yakyuu Special (Japan) (Rev 1)" },
    { 2u, 0xC4F9251Au, "Super Mario IV (Asia) (Ja) (Pirate)" },
    { 2u, 0xCA503F32u, "Itadaki Street - Watashi no Omise ni Yottette (Japan)" },
    { 2u, 0xCEE5857Bu, "Ninjara Hoi! (Japan)" },
    { 2u, 0xD0FBE052u, "Falling Tiles (World) (NESDev 2019) (Aftermarket) (Unl)" },
    { 2u, 0xD68A6F33u, "Dungeon Kid (Japan)" },
    { 2u, 0xD7CD7E8Eu, "Panesian Power (World) (NESDev 2011) (Aftermarket) (Unl)" },
    { 2u, 0xD89E5A67u, "Arkanoid (Japan) (En)" },
    { 2u, 0xDDCBDA16u, "Lightgun Game 2 in 1 - Tough Cop + Super Tough Cop (Taiwan) (En) (Unl)" },
    { 2u, 0xDE8FD935u, "To the Earth (USA)" },
    { 2u, 0xDF31B364u, "Cobra Mission (Taiwan) (En) (Unl)" },
    { 2u, 0xDF3E45D2u, "Mad City (Japan)" },
    { 2u, 0xE145B441u, "Day Dreamin' Davey (USA)" },
    { 2u, 0xE19293A2u, "Best Play Pro Yakyuu (Japan) (Rev 1)" },
    { 2u, 0xE85B4D3Du, "Hit Marmot (Taiwan) (En) (Unl)" },
    { 2u, 0xEDC3662Bu, "Operation Wolf (USA)" },
    { 2u, 0xFF24D794u, "Hogan's Alley (World)" },
    { 3u, 0x0B614307u, "RPG Tsukuru GB (Japan)" },
    { 3u, 0x219E42E3u, "Uchuujin Tanaka Tarou de - RPG Tsukuru GB 2 (Japan)" },
};
static const Uint32 s_AuroraNoIntroKnownSNES[] = { 0x07E01EF9u, 0x0F66698Bu, 0x11A6E64Bu, 0x125A0C22u, 0x12F0A699u, 0x17657DB6u, 0x19369514u, 0x19BDCB19u, 0x266C44EDu, 0x271E1D07u, 0x2AA7777Bu, 0x2B88BEE8u, 0x2E8034ABu, 0x31DAEBB2u, 0x32E29079u, 0x343E0DFBu, 0x36768B55u, 0x3AA01DBDu, 0x3ADEF543u, 0x3C49A285u, 0x3EAA5697u, 0x493FDB13u, 0x4CD45939u, 0x4EE705ABu, 0x50F2D1BCu, 0x50FEF979u, 0x51A5F489u, 0x525FFB26u, 0x52ADA404u, 0x531463E1u, 0x5441F25Bu, 0x547CF7B9u, 0x56410E5Eu, 0x5EBF7246u, 0x5F3E5279u, 0x64EDFC5Du, 0x66989491u, 0x675B6382u, 0x6838BE08u, 0x6E8A1081u, 0x75EEEB3Fu, 0x762AF827u, 0x77871727u, 0x7A33E836u, 0x7E97AE6Au, 0x82497F19u, 0x857980B2u, 0x8886396Eu, 0x916D9C92u, 0x94093298u, 0x9DDE5CEAu, 0xA20BE998u, 0xAF415C24u, 0xB0150052u, 0xB17E95D4u, 0xB9B9DF06u, 0xBC1A265Fu, 0xBE523800u, 0xC17ECBA5u, 0xC49D28A4u, 0xC6A95816u, 0xC79C123Fu, 0xC8002453u, 0xC946DCA0u, 0xCAF432AAu, 0xCB0653D0u, 0xCD80351Cu, 0xCD80DB86u, 0xCF98DDAAu, 0xD0DE3012u, 0xD138F224u, 0xD239398Fu, 0xD34C49B7u, 0xDAF285A5u, 0xDEF45776u, 0xE0BD6C71u, 0xE20870EEu, 0xE2DBAD76u, 0xE44F42BAu, 0xE5A57B12u, 0xECA2C2D1u, 0xEFE9E087u, 0xF1063FADu, 0xF1110FADu };
static const Uint32 s_AuroraNoIntroKnownNES[] = { 0x012E12E3u, 0x01B87025u, 0x04A6B46Du, 0x051E60C6u, 0x0719E982u, 0x090568DFu, 0x0AFB395Eu, 0x0E9428FBu, 0x0F263E59u, 0x143DF524u, 0x149C0EC3u, 0x19B0A9F1u, 0x1A5CE587u, 0x1BFE42ABu, 0x1CA9C322u, 0x1CAE8DEFu, 0x1DA88F85u, 0x1E6C3344u, 0x1EC1DFEBu, 0x1F6660E6u, 0x21DD2174u, 0x231BC76Eu, 0x23D17F5Eu, 0x24598791u, 0x27ACE333u, 0x283D7727u, 0x2A3CA509u, 0x2A6559A1u, 0x2CE31186u, 0x2F72A0BEu, 0x30D00EF7u, 0x327DDDECu, 0x3469B714u, 0x3488A174u, 0x392E268Cu, 0x39A18397u, 0x3BBFF3A6u, 0x3C04E8EFu, 0x3E58A87Eu, 0x3E85BA0Fu, 0x3F8BB92Du, 0x407449DAu, 0x4318A2F8u, 0x431A5F59u, 0x44340DA6u, 0x44BEB4B0u, 0x47C0935Bu, 0x47C2020Bu, 0x48C66CEBu, 0x497C6A69u, 0x498187B6u, 0x4A3D4790u, 0x4B143FB6u, 0x4D3982BCu, 0x4D68CFB1u, 0x4E959173u, 0x4FBBFA74u, 0x5112DC21u, 0x522EE20Fu, 0x524BC479u, 0x5529431Fu, 0x55397DB3u, 0x58600A77u, 0x58B2FE44u, 0x5B3B5BC1u, 0x5C123EF7u, 0x5CC1E2C6u, 0x5D4574E0u, 0x5E8C77DBu, 0x5EE6008Eu, 0x61061352u, 0x61A852EAu, 0x62AF1BC4u, 0x6332E4CAu, 0x63506FB4u, 0x64594DA3u, 0x6519CB3Bu, 0x65AA77CEu, 0x67751094u, 0x6AF3DEF8u, 0x70DF0D3Du, 0x73CCDAE0u, 0x73FB55ACu, 0x74BEA652u, 0x78B657ACu, 0x790B295Bu, 0x7A018E1Fu, 0x7BAF8142u, 0x7C4EBDACu, 0x7C899CFAu, 0x7CDF51D5u, 0x7D01D4E0u, 0x7FA2CC55u, 0x7FC220F7u, 0x803B9979u, 0x82908FF7u, 0x8373021Eu, 0x83EAF3B1u, 0x8507A4F9u, 0x851EB9BEu, 0x8A7D9467u, 0x8B7DA8B8u, 0x8C4D59D6u, 0x91467F41u, 0x93216279u, 0x96BE8381u, 0x974E8840u, 0xA0FBF02Eu, 0xA1430EEBu, 0xA1A33B85u, 0xA39A8063u, 0xA671DA25u, 0xA7C6C842u, 0xA7F8BBC8u, 0xAA65ADBFu, 0xAA9F9765u, 0xAEF00A33u, 0xB037246Du, 0xB0480AE9u, 0xB133CFA7u, 0xB3D92E78u, 0xB79F2651u, 0xB811C054u, 0xB8747ABFu, 0xB8B9ACA3u, 0xB9AB06AAu, 0xBBE40DC4u, 0xBC9BFFCBu, 0xBCFDD7DEu, 0xBEB8AB01u, 0xC0F0D838u, 0xC22BC87Bu, 0xC267D861u, 0xC2EF3422u, 0xC3C9D852u, 0xC3DE7C69u, 0xC49F6407u, 0xC4F9251Au, 0xC616BAD5u, 0xCA2C23E2u, 0xCA503F32u, 0xCDC69231u, 0xCEE5857Bu, 0xCFD8D4A5u, 0xD0FBE052u, 0xD15009CCu, 0xD5BCF1E5u, 0xD68A6F33u, 0xD7CD7E8Eu, 0xD8015A7Au, 0xD89E5A67u, 0xDC33AED8u, 0xDDCBDA16u, 0xDE8FD935u, 0xDF31B364u, 0xDF3E45D2u, 0xE145B441u, 0xE18CD9AAu, 0xE19293A2u, 0xE46C7D6Bu, 0xE5620994u, 0xE615C8DFu, 0xE85B4D3Du, 0xEDC3662Bu, 0xEEC7995Au, 0xF24C0B66u, 0xF27F9E88u, 0xF4E7A58Cu, 0xF5E62944u, 0xFA08CCBFu, 0xFD9E5DD6u, 0xFF24D794u };
static const Uint32 s_AuroraNoIntroKnownGBC[] = { 0x0B614307u, 0x219E42E3u };
static char s_AuroraNoIntroName[512] = {0};
static Uint8 s_AuroraNoIntroSystem = 0;

static unsigned char AuroraNoIntroLower(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 'a' - 'A') : c;
}

static Bool AuroraNoIntroNameEqual(const char *a, const char *b)
{
    if (!a || !b) return FALSE;
    while (*a && *b)
    {
        if (AuroraNoIntroLower((unsigned char)*a++) !=
            AuroraNoIntroLower((unsigned char)*b++)) return FALSE;
    }
    return (*a == 0 && *b == 0) ? TRUE : FALSE;
}

static void AuroraNoIntroDropExtension(char *name)
{
    char *dot = strrchr(name, '.');
    if (!dot || dot == name || !dot[1]) return;
    /* Um título como "Dr. Mario" não tem extensão aqui. */
    for (const char *p = dot + 1; *p; ++p)
        if (*p == ' ' || *p == '(' || *p == ')' || *p == '/' || *p == '\\')
            return;
    if (strlen(dot + 1) > 12u) return;
    *dot = 0;
}

void AuroraRomIdentitySetNoIntroName(Uint32 system, const char *path)
{
    s_AuroraNoIntroName[0] = 0;
    s_AuroraNoIntroSystem = 0;
    if (!path || !*path || system < 1u || system > 3u) return;
    const char *base = path;
    for (const char *p = path; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    const size_t n = strlen(base);
    if (!n || n >= sizeof(s_AuroraNoIntroName)) return;
    memcpy(s_AuroraNoIntroName, base, n + 1u);
    /* Ex.: "Game (USA).sfc.gz" -> "Game (USA)". ZIP usa o membro interno. */
    char *lastDot = strrchr(s_AuroraNoIntroName, '.');
    if (lastDot && AuroraNoIntroNameEqual(lastDot, ".gz"))
        *lastDot = 0;
    AuroraNoIntroDropExtension(s_AuroraNoIntroName);
    s_AuroraNoIntroSystem = (Uint8)system;
}

Uint32 AuroraRomIdentityCRCForDetection(Uint32 crc)
{
    if (!crc || !s_AuroraNoIntroSystem || !s_AuroraNoIntroName[0]) return crc;
    const Uint32 *known = NULL;
    size_t count = 0;
    switch (s_AuroraNoIntroSystem)
    {
        case 1: known = s_AuroraNoIntroKnownSNES;
                count = sizeof(s_AuroraNoIntroKnownSNES) / sizeof(Uint32); break;
        case 2: known = s_AuroraNoIntroKnownNES;
                count = sizeof(s_AuroraNoIntroKnownNES) / sizeof(Uint32); break;
        case 3: known = s_AuroraNoIntroKnownGBC;
                count = sizeof(s_AuroraNoIntroKnownGBC) / sizeof(Uint32); break;
        default: return crc;
    }
    /* Nenhum nome substitui uma identidade já reconhecida pelo CRC. */
    for (size_t i = 0; i < count; ++i)
        if (crc == known[i]) return crc;
    for (size_t i = 0; i < sizeof(s_AuroraNoIntroAliases) /
                             sizeof(s_AuroraNoIntroAliases[0]); ++i)
        if (s_AuroraNoIntroAliases[i].system == s_AuroraNoIntroSystem &&
            AuroraNoIntroNameEqual(s_AuroraNoIntroName,
                                   s_AuroraNoIntroAliases[i].name))
            return s_AuroraNoIntroAliases[i].crc;
    return crc;
}


/* AURORA_SAFE_FRAMESKIP_30FPS_FLOOR_V21_20261001
 *
 * Host-only catch-up scheduler with a hard presentation floor.
 *
 * The old scheduler accumulated multiple burst/flicker/recovery policies.
 * This version keeps only the measurements needed to answer one question:
 * would presenting this host tick knowingly miss the next VBlank budget?
 *
 * Skip budget: one token costs 5 units, each eligible host tick earns 2.
 * The bucket holds at most one token and two skips are never consecutive.
 * Under sustained debt this limits hidden presentations to <= 2/5:
 *   50 Hz host -> >= 30 visible fps
 *   ~60 Hz host -> >= 36 visible fps
 * The 5-tick ratio is odd, so presented-frame parity naturally walks instead
 * of phase-locking to every-other-frame sprite flicker; no separate flicker
 * protection state or forced parity swap is needed.
 *
 * Emulated CPU/audio time is never skipped. Only host video presentation and
 * its VBlank wait are omitted, exactly like the previous Safe Frameskip path.
 */
/* AURORA_V48_3_BOUND_NORMAL_HOST_DEBT_20261012
 * Compile-time A/B for the NORMAL Safe Frameskip host scheduler only.
 * All video buffers, audio timing and emulated clocks are unchanged. */
#ifndef AURORA_V48_3_BOUND_NORMAL_HOST_DEBT
#define AURORA_V48_3_BOUND_NORMAL_HOST_DEBT 1
#endif

static Int32  s_SafeFrameskipLevel = 1;
static Bool   s_SafeFrameskipSkipPresentation = FALSE;
static Bool   s_SafeFrameskipGameplayActive = FALSE;

static Uint32 s_SafeFrameskipAim = 0;
static Uint32 s_SafeFrameskipPeriod = 0;
static Uint32 s_SafeFrameskipSamples[3] = { 0, 0, 0 };
static Uint32 s_SafeFrameskipSampleCount = 0;
static Uint32 s_SafeFrameskipSamplePos = 0;
static Uint32 s_SafeFrameskipLastFlip = 0;
static Uint32 s_SafeFrameskipFrontendLastFlip = 0;

/* Take()->pre-VSync host work from the last visible gameplay tick. */
static Uint32 s_SafeFrameskipTickStart = 0;
static Uint32 s_SafeFrameskipPreFlip = 0;
static Uint32 s_SafeFrameskipLastPresentedWork = 0;
static Bool   s_SafeFrameskipLastPresentedWorkValid = FALSE;
static Bool   s_SafeFrameskipOverrunPending = FALSE;

/* AURORA_SAFE_FRAMESKIP_HEALTHY_REBASE_V22_20261004
 * A normal-policy skip arms one recovery proof. The next real
 * presented gameplay tick may clear stale Aim debt only if its
 * measured Take()->pre-VBlank work fits the learned period.
 * CRC-unlimited policy never arms this state. */
static Bool   s_SafeFrameskipRecoveryPending = FALSE;
/* AURORA_SAFE_FRAMESKIP_PRESENTED_RECOVERY_V35_20261015: only ACTUALLY PRESENTED, on-budget host ticks count. */
static Uint32 s_SafeFrameskipHealthyPresentCount = 0u;

/* Exact 2/5 token bucket. Capacity 6 preserves the one-unit remainder
 * when +2 credit crosses the 5-unit skip cost; <10 still banks at most one
 * complete skip. The no-consecutive guard remains the hard burst bound. */
static Uint32 s_SafeFrameskipCredit = 5;
static Bool   s_SafeFrameskipPreviousSkipped = FALSE;

/* AURORA_SAFE_FRAMESKIP_FLICKER_PHASE_SWAP_V21_1_20261001
 * Lightweight phase rotation only. Count ordinary debt-driven skip candidates.
 * Every 16th candidate is shown without spending its token. If debt persists,
 * the next tick naturally spends that still-full token, turning S/P into P/S
 * with no compensation flag and no sustained-throughput loss. */
static Uint32 s_SafeFrameskipPhaseSwapCount = 0;


/* AURORA_SAFE_FRAMESKIP_CRC_UNLIMITED_V21_2_20261001
 * Unlimited policy is selected once at ROM attach time. The allow-list switch
 * is therefore completely outside the host-tick hot path. During gameplay the
 * only policy test is one owner-pointer comparison. */
static const void *s_SafeFrameskipUnlimitedOwner = NULL;

static Bool _MainLoopSafeFrameskipCRCUsesUnlimited(Uint32 crc32)
{
    switch (crc32)
    {
        /* Generic CRC allow-list. Add future ROM identities here. */
        case 0xD34C49B7u:
        case 0xB0150052u:
        case 0xE5A57B12u:
        case 0x2B88BEE8u:
        case 0x531463E1u:
        case 0x31DAEBB2u:
        case 0xA20BE998u:
        case 0x493FDB13u:
        case 0xB9B9DF06u:
        /* AURORA_V24_AUDIO_FRAME_BUDGET_SONICMAX_AUDIT_20261001
         * Sonic 3D family intentionally NOT in unlimited/max Safe Frameskip.
         * Easy opt-in if testing changes the decision:
         *   case 0x44A2CA44u:  // Sonic 3D Blast / Flickies' Island
         *   case 0x3029C4F2u:  // Sonic 3D Director's Cut (Demo)
         *   case 0x9767E840u:  // Sonic 3D Director's Cut (v1.0 Final)
         */
            return TRUE;
        default:
            return FALSE;
    }
}

Uint32 MainLoopSafeFrameskipRomCRC32(const void *pDataVoid, Uint32 nBytes)
{
    static Uint32 table[256];
    static Bool tableReady = FALSE;
    const Uint8 *pData = (const Uint8 *)pDataVoid;
    Uint32 crc = 0xFFFFFFFFu;

    if (!pData || nBytes == 0u)
        return 0u;

    if (!tableReady)
    {
        for (Uint32 i = 0; i < 256u; ++i)
        {
            Uint32 c = i;
            for (Uint32 bit = 0; bit < 8u; ++bit)
                c = (c >> 1) ^ ((c & 1u) ? 0xEDB88320u : 0u);
            table[i] = c;
        }
        tableReady = TRUE;
    }

    while (nBytes--)
        crc = table[(crc ^ *pData++) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

void MainLoopSafeFrameskipSetRomIdentityCRC32(
    const void *pSystemOwner, Uint32 crc32)
{
    s_SafeFrameskipUnlimitedOwner =
        (pSystemOwner && _MainLoopSafeFrameskipCRCUsesUnlimited(
             AuroraRomIdentityCRCForDetection(crc32)))
            ? pSystemOwner : NULL;

    /* Media attach is a clean policy boundary. Do not inherit bucket or
     * anti-flicker phase from the previous cartridge in the same core. */
    s_SafeFrameskipCredit = 5u;
    s_SafeFrameskipPreviousSkipped = FALSE;
    s_SafeFrameskipPhaseSwapCount = 0u;
    s_SafeFrameskipRecoveryPending = FALSE;
    s_SafeFrameskipHealthyPresentCount = 0u;
}

/* UI pause preserves scheduler phase without sampling time spent in menus. */
static Bool        s_SafeFrameskipCommittedValid = FALSE;
static Int32       s_SafeFrameskipCommittedAimPhase = 0;
static const void *s_SafeFrameskipCommittedSystem = NULL;
static Uint32      s_SafeFrameskipCommittedFrame = 0;
static Bool        s_SafeFrameskipUiFreezeValid = FALSE;
static Bool        s_SafeFrameskipUiFreezeHadAim = FALSE;
static Int32       s_SafeFrameskipUiFreezeAimPhase = 0;
static const void *s_SafeFrameskipUiFreezeSystem = NULL;
static Uint32      s_SafeFrameskipUiFreezeFrame = 0;
static Bool        s_SafeFrameskipUiResumePending = FALSE;
static Int32       s_SafeFrameskipUiResumeAimPhase = 0;

/* Legacy CD window ABI; modern streaming-CD paths normally disallow FS. */
static Bool s_SafeFrameskipCdAudioWindowRequested = FALSE;

static Uint32 _MainLoopSafeFrameskipMedian3(Uint32 a, Uint32 b, Uint32 c)
{
    if (a > b) { const Uint32 x = a; a = b; b = x; }
    if (b > c) { const Uint32 x = b; b = c; c = x; }
    if (a > b) { const Uint32 x = a; a = b; b = x; }
    return b;
}

static void _MainLoopSafeFrameskipInvalidateCommitted(void)
{
    s_SafeFrameskipCommittedValid = FALSE;
    s_SafeFrameskipCommittedAimPhase = 0;
    s_SafeFrameskipCommittedSystem = NULL;
    s_SafeFrameskipCommittedFrame = 0;
}

static void _MainLoopSafeFrameskipResetRuntime(void)
{
    s_SafeFrameskipAim = 0;
    s_SafeFrameskipSkipPresentation = FALSE;
    s_SafeFrameskipTickStart = 0;
    s_SafeFrameskipPreFlip = 0;
    s_SafeFrameskipLastPresentedWork = 0;
    s_SafeFrameskipLastPresentedWorkValid = FALSE;
    s_SafeFrameskipOverrunPending = FALSE;
    s_SafeFrameskipRecoveryPending = FALSE;
    s_SafeFrameskipHealthyPresentCount = 0u;
    s_SafeFrameskipCredit = 5;
    s_SafeFrameskipPreviousSkipped = FALSE;
    s_SafeFrameskipPhaseSwapCount = 0;
    s_SafeFrameskipUiResumePending = FALSE;
    s_SafeFrameskipUiResumeAimPhase = 0;
    _MainLoopSafeFrameskipInvalidateCommitted();
}

static void _MainLoopSafeFrameskipCommitGameplayBoundary(Uint32 now)
{
    const void *system = (const void *)_pSystem;

    if (system != NULL && s_SafeFrameskipAim != 0u)
    {
        s_SafeFrameskipCommittedValid = TRUE;
        s_SafeFrameskipCommittedAimPhase =
            (Int32)(s_SafeFrameskipAim - now);
        s_SafeFrameskipCommittedSystem = system;
        s_SafeFrameskipCommittedFrame = (Uint32)_pSystem->GetFrame();
    }
    else
    {
        _MainLoopSafeFrameskipInvalidateCommitted();
    }
}

static void _MainLoopSafeFrameskipLearn(Uint32 delta, Bool gameplay)
{
    if (delta == 0u)
        return;

    if (s_SafeFrameskipPeriod == 0u)
    {
        if (s_SafeFrameskipSampleCount != 0u)
        {
            const Uint32 lastPos = (s_SafeFrameskipSamplePos + 2u) % 3u;
            const Uint32 ref = s_SafeFrameskipSamples[lastPos];
            if ((Uint64)delta * 4u < (Uint64)ref * 3u ||
                (Uint64)delta * 4u > (Uint64)ref * 5u)
            {
                s_SafeFrameskipSamples[0] = delta;
                s_SafeFrameskipSamples[1] = 0;
                s_SafeFrameskipSamples[2] = 0;
                s_SafeFrameskipSamplePos = 1u;
                s_SafeFrameskipSampleCount = 1u;
                return;
            }
        }

        s_SafeFrameskipSamples[s_SafeFrameskipSamplePos] = delta;
        s_SafeFrameskipSamplePos = (s_SafeFrameskipSamplePos + 1u) % 3u;
        if (s_SafeFrameskipSampleCount < 3u)
            ++s_SafeFrameskipSampleCount;
        if (s_SafeFrameskipSampleCount < 3u)
            return;

        s_SafeFrameskipPeriod = _MainLoopSafeFrameskipMedian3(
            s_SafeFrameskipSamples[0],
            s_SafeFrameskipSamples[1],
            s_SafeFrameskipSamples[2]);
        _MainLoopSafeFrameskipResetRuntime();
        return;
    }

    if (gameplay)
    {
        if ((Uint64)delta * 4u < (Uint64)s_SafeFrameskipPeriod * 3u ||
            (Uint64)delta * 4u > (Uint64)s_SafeFrameskipPeriod * 5u)
            return;
    }
    else
    {
        if ((Uint64)delta * 3u < (Uint64)s_SafeFrameskipPeriod * 2u ||
            (Uint64)delta * 2u > (Uint64)s_SafeFrameskipPeriod * 3u)
            return;
    }

    s_SafeFrameskipSamples[s_SafeFrameskipSamplePos] = delta;
    s_SafeFrameskipSamplePos = (s_SafeFrameskipSamplePos + 1u) % 3u;
    s_SafeFrameskipPeriod = _MainLoopSafeFrameskipMedian3(
        s_SafeFrameskipSamples[0],
        s_SafeFrameskipSamples[1],
        s_SafeFrameskipSamples[2]);
}

Int32 MainLoopSafeFrameskipGetLevel(void)
{
    return s_SafeFrameskipLevel > 0 ? 1 : 0;
}

void MainLoopSafeFrameskipSetLevel(Int32 level)
{
#if AURORA_FRONTEND_PROFILER
    s_SafeFrameskipLevel = level > 0 ? 1 : 0;
#else /* AURORA_V44_CLEAN_SAFE_FS_ON_20261011: clean always ON, including old video.cfg */
    (void)level;
    s_SafeFrameskipLevel = 1;
#endif
    s_SafeFrameskipCdAudioWindowRequested = FALSE;
    s_SafeFrameskipUiFreezeValid = FALSE;
    s_SafeFrameskipUiFreezeHadAim = FALSE;
    s_SafeFrameskipUiFreezeAimPhase = 0;
    s_SafeFrameskipUiFreezeSystem = NULL;
    s_SafeFrameskipUiFreezeFrame = 0;
    s_SafeFrameskipLastFlip = 0;
    s_SafeFrameskipFrontendLastFlip = 0;
    _MainLoopSafeFrameskipResetRuntime();
}

Bool MainLoopSafeFrameskipGetEnabled(void)
{
    return s_SafeFrameskipLevel > 0 ? TRUE : FALSE;
}

void MainLoopSafeFrameskipSetEnabled(Bool enabled)
{
    MainLoopSafeFrameskipSetLevel(enabled ? 1 : 0);
}

void MainLoopSafeFrameskipSetGameplayActive(Bool active)
{
    const void *system = (const void *)_pSystem;
    const Uint32 frame = _pSystem ? (Uint32)_pSystem->GetFrame() : 0u;

    if (active && s_SafeFrameskipGameplayActive)
        return;

    if (!active)
    {
        if (s_SafeFrameskipGameplayActive)
        {
            const Bool sameCommitted =
                (s_SafeFrameskipCommittedValid && system != NULL &&
                 s_SafeFrameskipCommittedSystem == system &&
                 s_SafeFrameskipCommittedFrame == frame) ? TRUE : FALSE;

            s_SafeFrameskipGameplayActive = FALSE;
            s_SafeFrameskipHealthyPresentCount = 0u;
            s_SafeFrameskipUiFreezeValid = (system != NULL) ? TRUE : FALSE;
            s_SafeFrameskipUiFreezeHadAim = sameCommitted;
            s_SafeFrameskipUiFreezeAimPhase =
                sameCommitted ? s_SafeFrameskipCommittedAimPhase : 0;
            s_SafeFrameskipUiFreezeSystem = system;
            s_SafeFrameskipUiFreezeFrame = frame;

            s_SafeFrameskipSkipPresentation = FALSE;
            s_SafeFrameskipPreviousSkipped = FALSE;
            s_SafeFrameskipLastFlip = 0;
            s_SafeFrameskipFrontendLastFlip = 0;
            s_SafeFrameskipTickStart = 0;
            s_SafeFrameskipPreFlip = 0;
            return;
        }

        if (s_SafeFrameskipUiFreezeValid &&
            (system == NULL || system != s_SafeFrameskipUiFreezeSystem ||
             frame != s_SafeFrameskipUiFreezeFrame))
        {
            s_SafeFrameskipUiFreezeValid = FALSE;
            s_SafeFrameskipUiFreezeHadAim = FALSE;
            s_SafeFrameskipUiFreezeAimPhase = 0;
            s_SafeFrameskipUiFreezeSystem = system;
            s_SafeFrameskipUiFreezeFrame = frame;
            s_SafeFrameskipCdAudioWindowRequested = FALSE;
            _MainLoopSafeFrameskipResetRuntime();
        }
        return;
    }

    if (!s_SafeFrameskipGameplayActive)
    {
        const Bool samePausedState =
            (s_SafeFrameskipUiFreezeValid && system != NULL &&
             system == s_SafeFrameskipUiFreezeSystem &&
             frame == s_SafeFrameskipUiFreezeFrame) ? TRUE : FALSE;

        if (samePausedState && s_SafeFrameskipUiFreezeHadAim)
        {
            s_SafeFrameskipUiResumePending = TRUE;
            s_SafeFrameskipUiResumeAimPhase =
                s_SafeFrameskipUiFreezeAimPhase;
        }
        else if (!samePausedState)
        {
            s_SafeFrameskipCdAudioWindowRequested = FALSE;
            _MainLoopSafeFrameskipResetRuntime();
        }
        else
        {
            s_SafeFrameskipAim = 0u;
        }

        s_SafeFrameskipUiFreezeValid = FALSE;
        s_SafeFrameskipUiFreezeHadAim = FALSE;
        s_SafeFrameskipUiFreezeAimPhase = 0;
        s_SafeFrameskipUiFreezeSystem = NULL;
        s_SafeFrameskipUiFreezeFrame = 0;
        s_SafeFrameskipSkipPresentation = FALSE;
        s_SafeFrameskipPreviousSkipped = FALSE;
        s_SafeFrameskipLastFlip = 0;
        s_SafeFrameskipFrontendLastFlip = 0;
        s_SafeFrameskipTickStart = 0;
        s_SafeFrameskipPreFlip = 0;
        s_SafeFrameskipHealthyPresentCount = 0u;
        s_SafeFrameskipGameplayActive = TRUE;
    }
}

void MainLoopSafeFrameskipRequestCdAudioWindow(void)
{
    if (s_SafeFrameskipLevel > 0)
        s_SafeFrameskipCdAudioWindowRequested = TRUE;
}

void MainLoopSafeFrameskipCancelCdAudioWindow(void)
{
    s_SafeFrameskipCdAudioWindowRequested = FALSE;
}

Bool MainLoopSafeFrameskipTake(Bool allowed)
{
    Bool skip = FALSE;
    Bool meaningfulDebt = FALSE;
    Bool measuredOverrunForDiag = FALSE;
    Int32 diffForDiag = 0;
    Uint32 now;

    s_SafeFrameskipSkipPresentation = FALSE;

    if (!allowed || s_SafeFrameskipLevel <= 0)
    {
        s_SafeFrameskipCdAudioWindowRequested = FALSE;
        _MainLoopSafeFrameskipResetRuntime();
        return FALSE;
    }

    if (s_SafeFrameskipPeriod == 0u || s_SafeFrameskipSampleCount < 3u)
    {
        s_SafeFrameskipPreviousSkipped = FALSE;
        return FALSE;
    }

    now = ProfCtrGetCycle();
    s_SafeFrameskipTickStart = now;

    if (s_SafeFrameskipUiResumePending)
    {
        s_SafeFrameskipAim =
            now + (Uint32)s_SafeFrameskipUiResumeAimPhase;
        s_SafeFrameskipUiResumePending = FALSE;
        s_SafeFrameskipUiResumeAimPhase = 0;
    }

    if (s_SafeFrameskipAim == 0u)
        s_SafeFrameskipAim = now;

    /* AURORA_V48_3_BOUND_NORMAL_HOST_DEBT_20261012
     * The 32-bit COP0 host counter and the 32-bit Aim are modular.
     * The signed (Aim - now) check below only identifies elapsed debt
     * unambiguously while it stays below 2^31 counts.  When the normal
     * 2/5 presentation floor is saturated, work can exceed a period
     * indefinitely and Aim falls farther behind with every host tick.
     * Limit stale Aim to eight measured periods while it is still
     * unambiguously negative.  This keeps ordinary debt-driven skip
     * decisions TRUE, including after the clamp: eight periods is
     * far above any normal scheduler catch-up threshold.
     * Do not modify CRC-unlimited policy, throttle, audio or core time.
     * No unbounded loop, no 64-bit math on the normal fast path. */
#if AURORA_V48_3_BOUND_NORMAL_HOST_DEBT
    if (s_SafeFrameskipPeriod != 0u &&
        s_SafeFrameskipPeriod <= 0x0FFFFFFFu &&
        (s_SafeFrameskipUnlimitedOwner == NULL ||
         s_SafeFrameskipUnlimitedOwner != (const void *)_pSystem))
    {
        const Uint32 maxDebt = s_SafeFrameskipPeriod * 8u;
        const Int32 phase = (Int32)(s_SafeFrameskipAim - now);
        if (phase < 0 && (Uint32)(now - s_SafeFrameskipAim) > maxDebt)
            s_SafeFrameskipAim = now - maxDebt;
    }
#endif

    {
        const Int32 diff = (Int32)(s_SafeFrameskipAim - now);
        const Bool measuredOverrun = s_SafeFrameskipOverrunPending;
        diffForDiag = diff;
        measuredOverrunForDiag = measuredOverrun;
        s_SafeFrameskipOverrunPending = FALSE;

        if (measuredOverrun)
            meaningfulDebt = TRUE;

        if (diff < 0)
        {
            const Uint32 lateness = 0u - (Uint32)diff;
            if (s_SafeFrameskipLastPresentedWorkValid)
            {
                if ((Uint64)lateness +
                    (Uint64)s_SafeFrameskipLastPresentedWork >=
                    (Uint64)s_SafeFrameskipPeriod)
                    meaningfulDebt = TRUE;
            }
            else if (lateness >= s_SafeFrameskipPeriod)
            {
                meaningfulDebt = TRUE;
            }
        }
    }

    /* AURORA_SAFE_FRAMESKIP_BALANCED_RECOVERY_V23_20261005
     *
     * v22 already gave us the important safety proof: a REAL presented frame
     * is measured after a normal-policy skip. Keep that proof, but do not
     * throw away residual Aim debt too early.
     *
     * During an armed recovery only, more than half a learned host period of
     * residual lateness is still actionable debt. Below that threshold normal
     * VBlank cadence can absorb the phase error, so disarm recovery without
     * manufacturing another skip. The public 2/5 token bucket and the
     * no-consecutive-skip rule below remain untouched. */
    if (s_SafeFrameskipRecoveryPending &&
        s_SafeFrameskipPeriod > 0u)
    {
        const Int32 recoveryThreshold =
            (s_SafeFrameskipPeriod > 1u)
                ? (Int32)(s_SafeFrameskipPeriod >> 1)
                : 1;

        if (diffForDiag < -recoveryThreshold)
            meaningfulDebt = TRUE;
        else if (!measuredOverrunForDiag)
            s_SafeFrameskipRecoveryPending = FALSE;
    }

    /* AURORA_SAFE_FRAMESKIP_CRC_UNLIMITED_V21_2_20261001
     * The CRC allow-list was resolved at media attach time. No CRC, table or
     * switch work occurs here: only this owner-pointer comparison. */
    const Bool unlimitedSkip =
        (s_SafeFrameskipUnlimitedOwner != NULL &&
         s_SafeFrameskipUnlimitedOwner == (const void *)_pSystem)
            ? TRUE : FALSE;

    /* Normal policy remains the exact v21.1 2/5 bucket. Unlimited entries do
     * not spend or maintain bucket credit because their only bound is real
     * scheduler debt plus the periodic anti-flicker visible phase break. */
    if (!unlimitedSkip && s_SafeFrameskipCredit < 6u)
    {
        s_SafeFrameskipCredit += 2u;
        if (s_SafeFrameskipCredit > 6u)
            s_SafeFrameskipCredit = 6u;
    }

    const Bool skipCandidate = unlimitedSkip
        ? ((meaningfulDebt || s_SafeFrameskipCdAudioWindowRequested) ? TRUE : FALSE)
        : (((meaningfulDebt || s_SafeFrameskipCdAudioWindowRequested) &&
            !s_SafeFrameskipPreviousSkipped &&
            s_SafeFrameskipCredit >= 5u) ? TRUE : FALSE);

    if (skipCandidate)
    {
        Bool phaseSwapPresent = FALSE;

        /* Retain v21.1's sprite/flicker phase protection even in unlimited
         * mode. CD-audio refill windows are never delayed by this guard. */
        if (meaningfulDebt && !s_SafeFrameskipCdAudioWindowRequested)
        {
            ++s_SafeFrameskipPhaseSwapCount;
            if (s_SafeFrameskipPhaseSwapCount >= 16u)
            {
                s_SafeFrameskipPhaseSwapCount = 0u;
                phaseSwapPresent = TRUE;
            }
        }

        if (!phaseSwapPresent)
        {
            if (!unlimitedSkip)
            {
                s_SafeFrameskipCredit -= 5u;
                s_SafeFrameskipRecoveryPending = TRUE;
            }
            s_SafeFrameskipPreviousSkipped = TRUE;
            s_SafeFrameskipCdAudioWindowRequested = FALSE;
            skip = TRUE;
        }
        else
        {
            /* Keep the token untouched in normal mode; under continuing debt
             * the next eligible tick naturally performs the phase swap. */
            s_SafeFrameskipPreviousSkipped = FALSE;
        }
    }
    else
    {
        s_SafeFrameskipPreviousSkipped = FALSE;
    }

#if AURORA_FRONTEND_PROFILER
    AuroraFrontendProfilerSafeFrameskipDecision(
        s_SafeFrameskipPeriod, diffForDiag,
        s_SafeFrameskipLastPresentedWork, meaningfulDebt,
        measuredOverrunForDiag, unlimitedSkip, skip,
        s_SafeFrameskipRecoveryPending);
#endif

    s_SafeFrameskipAim += s_SafeFrameskipPeriod;
    s_SafeFrameskipSkipPresentation = skip;
    return skip;
}

Bool MainLoopSafeFrameskipConsumePresentationSkip(void)
{
    const Bool skip = s_SafeFrameskipSkipPresentation;
    s_SafeFrameskipSkipPresentation = FALSE;
    return skip;
}

static void _MainLoopSafeFrameskipAfterFlip(void)
{
    const Uint32 now = ProfCtrGetCycle();
    const Bool gameplay =
        (!_bMenu && _pSystem && !_MainLoop_BlackScreen) ? TRUE : FALSE;
    const Uint32 preFlip = s_SafeFrameskipPreFlip;

    s_SafeFrameskipPreFlip = 0;

    if (gameplay)
    {
        const Bool hostWorkValid =
            (s_SafeFrameskipTickStart != 0u && preFlip != 0u) ? TRUE : FALSE;
        const Uint32 hostWork =
            hostWorkValid ? (preFlip - s_SafeFrameskipTickStart) : 0u;
        const Bool recoveryWasPending =
            s_SafeFrameskipRecoveryPending;
        Bool recoveryRebased = FALSE;

        s_SafeFrameskipFrontendLastFlip = 0;

        if (hostWorkValid)
        {
            s_SafeFrameskipLastPresentedWork = hostWork;
            s_SafeFrameskipLastPresentedWorkValid = TRUE;
            if (s_SafeFrameskipPeriod > 0u &&
                (Uint64)hostWork > (Uint64)s_SafeFrameskipPeriod)
                s_SafeFrameskipOverrunPending = TRUE;
        }

        if (s_SafeFrameskipLastFlip != 0u)
        {
            const Uint32 delta = now - s_SafeFrameskipLastFlip;
            if (s_SafeFrameskipPeriod == 0u ||
                (hostWorkValid && hostWork <= s_SafeFrameskipPeriod))
                _MainLoopSafeFrameskipLearn(delta, TRUE);
        }

        /* AURORA_SAFE_FRAMESKIP_HEALTHY_REBASE_V22_20261004
         * A skipped NORMAL-policy tick is followed by a real presentation
         * because the 2/5 policy forbids consecutive skips. v3.3 deliberately
         * asks for >=25% host-work headroom before declaring the catch-up
         * complete immediately. A merely just-under-budget frame is stable,
         * but not strong proof that residual Aim debt has disappeared.
         *
         * Sustained/near-sustained load keeps recovery armed; Take() then uses
         * the v3.3 half-period residual threshold. CRC unlimited mode never
         * arms recovery, so Top Gear/Top Racer max policy is unchanged. */
        /* AURORA_SAFE_FRAMESKIP_PRESENTED_RECOVERY_V35_20261015
         * A transient overload can leave Aim eight host periods late.
         * Demanding 25% headroom forever after that overload can
         * lock a now-on-budget scene into persistent frameskip.
         * Preserve the original immediate <=75% proof, but also
         * accept THREE consecutive real presentations whose
         * measured pre-VBlank host work fits in ONE full period.
         * Hidden ticks are NEVER counted. An over-budget/invalid
         * presentation clears the streak. No new hot-path timer,
         * new clock, token policy or emulated-state modification.
         */
        if (recoveryWasPending && hostWorkValid &&
            s_SafeFrameskipPeriod != 0u &&
            hostWork <= s_SafeFrameskipPeriod)
        {
            if (s_SafeFrameskipHealthyPresentCount < 3u)
                ++s_SafeFrameskipHealthyPresentCount;
        }
        else
        {
            s_SafeFrameskipHealthyPresentCount = 0u;
        }

        if (recoveryWasPending &&
            hostWorkValid &&
            s_SafeFrameskipPeriod > 0u &&
            (((Uint64)hostWork * 4u <=
              (Uint64)s_SafeFrameskipPeriod * 3u) ||
             s_SafeFrameskipHealthyPresentCount >= 3u))
        {
            s_SafeFrameskipAim = now + s_SafeFrameskipPeriod;
            s_SafeFrameskipOverrunPending = FALSE;
            s_SafeFrameskipRecoveryPending = FALSE;
            s_SafeFrameskipPreviousSkipped = FALSE;
            s_SafeFrameskipHealthyPresentCount = 0u;
            recoveryRebased = TRUE;
        }

#if AURORA_FRONTEND_PROFILER
        AuroraFrontendProfilerSafeFrameskipRecovery(
            hostWork, recoveryWasPending, recoveryRebased);
#endif

        s_SafeFrameskipLastFlip = now;
        s_SafeFrameskipTickStart = 0;
    }
    else if (!_pSystem)
    {
        s_SafeFrameskipLastFlip = 0;
        if (s_SafeFrameskipFrontendLastFlip != 0u)
            _MainLoopSafeFrameskipLearn(
                now - s_SafeFrameskipFrontendLastFlip, FALSE);
        s_SafeFrameskipFrontendLastFlip = now;
        s_SafeFrameskipTickStart = 0;
    }
    else
    {
        s_SafeFrameskipLastFlip = 0;
        s_SafeFrameskipFrontendLastFlip = 0;
        s_SafeFrameskipTickStart = 0;
        s_SafeFrameskipPreFlip = 0;
    }
}

void MainLoopRender()
{
	static Uint32 _iFrame=0;
#if AURORA_FRONTEND_PROFILER
    AuroraFrontendProfilerRenderBegin();
#endif

        /* AURORA_SAFE_FRAMESKIP_UI_EDGE_RENDER_FALLBACK_V9_20260926
         *
         * Some modal/status/frontend paths render directly and therefore do
         * not pass through MainLoopProcess(). Observe the gameplay/UI state
         * again here so such a path cannot bypass the V8.1 timing barrier.
         *
         * Normal gameplay already observed the same state before Take(); the
         * observer is edge-triggered/idempotent, so this is a zero-mutation
         * no-op on the ordinary hot path.
         *
         * IMPORTANT: this runs BEFORE consuming a queued presentation skip.
         * If UI became active after the scheduler decision, the V8.1 edge
         * reset cancels that stale one-shot instead of hiding the first UI
         * presentation or carrying its phase through the menu.
         */
        MainLoopSafeFrameskipSetGameplayActive(
            (!_bMenu && _pSystem && !_MainLoop_BlackScreen)
                ? TRUE : FALSE);

        /* AURORA_SAFE_FRAMESKIP_PICODRIVE_AUTO_V1: standalone PicoDrive skip path has no
         * finalize/present/flip/VBlank wait. Core/audio already ran. */
        if (MainLoopSafeFrameskipConsumePresentationSkip())
        {
            if (!_bMenu && _pSystem && !_MainLoop_BlackScreen)
            {
                /* AURORA_CD_AUDIO_STREAM_V2_SKIP_AUDIO_CATCHUP_20260829
                 * The presentation/VBlank wait is already being skipped, so
                 * spend that recovered host time on at most one EXTRA
                 * nonblocking async-audio drain. No wait_audio(), no PCM drop.
                 * With no backlog the second call returns immediately. */
#if AURORA_C4_PROFILER
                const Uint32 c4SkipAud=g_AuroraC4ProfilerActive?ProfCtrGetCycle():0u;
#endif
#if AURORA_FRONTEND_PROFILER
                AuroraFrontendProfilerAudioDrainBegin();
#endif
                Aud_BufferedAsyncStart();
                Aud_BufferedAsyncStart();
#if AURORA_FRONTEND_PROFILER
                AuroraFrontendProfilerAudioDrainEnd();
#endif
#if AURORA_C4_PROFILER
                if(g_AuroraC4ProfilerActive)AuroraC4ProfilerPostAudio((Uint32)(ProfCtrGetCycle()-c4SkipAud),TRUE);
#endif

                /* V15 completed hidden-host-tick boundary. Core/audio have
                 * already advanced; no VBlank/presentation wait follows. */
                _MainLoopSafeFrameskipCommitGameplayBoundary(
                    ProfCtrGetCycle());
            }
#if AURORA_FRONTEND_PROFILER
            AuroraFrontendProfilerRenderEnd();
#endif
            ++_iFrame;
            return;
        }

        /* AURORA_SEGA_UI_PRESENTATION_V3
         *
         * Keep two different host presentations:
         *
         *   UI / browser / prompts -> Aurora's normal 256-wide raster
         *   plain MD gameplay      -> native 320-wide raster in 240p
         *   SMS and other systems  -> normal 256-wide raster
         *
         * The important distinction from the old late-init path is that
         * the FIRST MD 320 raster is still selected in mainloop_load.cpp
         * before PicoDrive is initialised. These later switches happen
         * only when entering/leaving Aurora's internal UI.
         *
         * In 480i/1080i MainLoopEnsureGameplayRasterWidth() does not
         * rebuild the GS; those modes keep their normal 640 framebuffer.
         */
        const Bool bMdVideo =
            (_pSystem == _pSega &&
             PicoDriveBridge_IsMegaDriveVideo()) ? TRUE : FALSE;

        {
            Int32 wantedRaster = 256;

            /* AURORA_SEGA_CD_32X_MD_SCALING_V2R1_20260828
             * Cartridge MD, Sega CD and 32X share H32/H40 presentation. */
            if (bMdVideo)
                wantedRaster = 320;


            /* AURORA_PCE_SSF2_FINAL_R2_20260913_PCE_FIXED512_MENU256
             * Real PS2: rebuilding 512->256 while Beetle/PCE-CD is alive can
             * destroy the live GS/VRAM epoch and hang the console.  Keep the
             * physical PCE raster at 512 for the entire core lifetime.  The
             * menu is a 256-sample presentation inside that backing instead. */
            if (_pSystem == _pPce)
                wantedRaster = 512;
            /* Menu/prompts use exact 256-source integer presentation on the
             * still-alive 320 framebuffer, not 256->320 resampling. */
            /* AURORA_PCE_SSF2_FINAL_R2_20260913_PCE_FIXED512_MENU256 */
            GSK_SetUi256OnWideFramebuffer(
                (_bMenu && (bMdVideo || _pSystem == _pPce)) ? 1 : 0);

            if (_bMenu)
                GSK_SetNative240pPar(0);

            if (!MainLoopEnsureGameplayRasterWidth(wantedRaster))
            {
                printf("[video] warning: could not switch to %d-wide presentation\n",
                       (int)wantedRaster);
            }

            /* AURORA_PCE_SSF2_FINAL_20260913_PCE_MENU_CLEAR_GAME_WINDOW
             * A gameplay crop/window never belongs to Aurora UI. */
            if (_pSystem != _pPce || _bMenu || _MainLoop_BlackScreen)
                GSK_Clear240pVisibleWindow();


        }



        /* AURORA_GLOBAL_PERF_V2_20260922
         * Read-only presentation facts for this host render. Query each bridge
         * at most once; no persistent capability cache is introduced. */
        const Bool bSegaDirectGs =
            (!_MainLoop_BlackScreen && _pSystem == _pSega &&
             PicoDriveBridge_CanDirectGsVideo()) ? TRUE : FALSE;
        const Bool bPceDirectGs =
            (!_MainLoop_BlackScreen && _pSystem == _pPce &&
             PceBridge_CanDirectGsVideo()) ? TRUE : FALSE;
        const Bool bGbaDirectGs =
            (!_MainLoop_BlackScreen && _pSystem == _pGba && _pGba &&
             _pGba->CanDirectGsVideo()) ? TRUE : FALSE;

        /* AURORA_GPSP_GBA_V14_NATIVE_SQUARE_20260911
         * Both standalone GB/GBC and GBA LCDs use square source pixels.
         * Reuse v13's presentation-only policy: no core framebuffer
         * resampling, no effect on dynamic SGB, menus or other systems. */
        const Bool bSquareHandheldGameplay =
            (!_bMenu && !_MainLoop_BlackScreen &&
             ((_pSystem == _pGb && _pGb &&
               _pGb->UsesSquarePixelPresentation()) ||
              (_pSystem == _pGba))) ? TRUE : FALSE;
        GSK_SetGbSquarePixelPresentation(bSquareHandheldGameplay ? 1 : 0);

        /*
         * NES/SNES/8-bit-console 240p: keep the framebuffer strictly 256x240 (1 source
         * pixel = 1 framebuffer pixel) and correct horizontal size at
         * the PCRTC level instead. This avoids uneven pixel widths
         * caused by scaling 256 pixels into a smaller PolyRect.
         */
        static int s_native240pPar = -1;
        int native240pPar =
            (g_GskVideoMode == GSK_VIDMODE_240P &&
             (_pSystem == _pNes ||
              _pSystem == _pFds || /* AURORA_FCEUMM_FDS_V0_5_RENDER */
              _pSystem == _pSnes ||
              (_pSystem == _pSega &&
               (bMdVideo ||
                PicoDriveBridge_IsMasterSystem()))) &&
             !_bMenu) ? 1 : 0;

        if (native240pPar != s_native240pPar)
        {
            GSK_SetNative240pPar(native240pPar);
            s_native240pPar = native240pPar;
        }

        /* AURORA_MD_YOFFSET_MINUS9_V1
         * Plain MD only: effective Y = configured menu Y - 9. */
        {
            static int s_lastMdYBias = 9999;
            int mdYBias = 0;

            if (mdYBias != s_lastMdYBias)
            {
                GSK_SetGameplayYOffsetBias(mdYBias);
                s_lastMdYBias = mdYBias;
            }
        }



    /* Re-anchor FRAME_1 to gsKit's current draw buffer before any
       primitive runs this frame. The legacy GS_SetDrawFB used to do
       this implicitly per frame; gsKit_sync_flip only swaps the
       display buffer, not the draw buffer. Without this, prims drew
       to a stale (or, after the SNES blender ran, completely wrong)
       buffer and the visible framebuffer flickered black on every
       other frame. See gskit_backend.h for the longer rationale. */
    /* AURORA_GS_PARTIAL_GAMEPLAY_CLEAR_V1
     * Only enable the reduced clear when this frame is guaranteed to draw the
     * normal game output texture. Menu, boot and black-screen frames retain
     * the complete physical clear. */
    /* AURORA_PD_DIRECT_MD_SKIP_CLEAR_V3_RENDER_20260821
     * DrawDirectGs() for plain MD always emits an opaque backdrop over
     * the entire transformed 256x240 canvas, which is the entire active
     * framebuffer in 240p/480i/1080i. Avoid clearing pixels that are
     * guaranteed to be replaced later in this same frame. */
    /* AURORA_PD_DIRECT_8BIT_SKIP_CLEAR_V4_20260821 */
    /* AURORA_PCE_EXPERIMENTAL_V13_NATIVE_PAR_REBUILD
     *
     * PCE base geometry is 256x240 with a narrower-than-4:3 presentation.
     * Use the same native-240p PCRTC technique already used by Aurora's
     * NES/SNES path: change scanout magnification, NOT framebuffer sampling.
     * Every one of the 256 source columns is still scanned exactly once.
     *
     * Keep menu/black-screen presentation untouched. The loaded system remains
     * _pPce while Aurora's menu is open, so explicitly turn the PCE-only PAR
     * correction back off there. GSK_SetNative240pPar internally affects only
     * the 240p display mode.
     */
    if (_pSystem == _pPce)
    {
        /* AURORA_PCE_CRT_OVERSCAN_352_V9_20260830
         * PCE has explicit 256/352/512 PCRTC profiles in gskit_backend.
         * Do not feed the fixed 512 framebuffer through NES/SNES PAR math. */
        GSK_SetNative240pPar(0);
    }

#if AURORA_FRONTEND_PROFILER
    AuroraFrontendProfilerRenderSetupBegin();
#endif
    GSK_SetGameplaySkipClear(
        (!_bMenu && !_MainLoop_BlackScreen &&
         ((bSegaDirectGs &&
           (bMdVideo ||
            g_GskVideoMode == GSK_VIDMODE_240P)) ||
          bPceDirectGs ||
          (bGbaDirectGs &&
           GSK_GetActiveVideoMode() == GSK_VIDMODE_240P))) ? TRUE : FALSE); /* AURORA_GPSP_GBA_V16_DIRECT_GS_CT16_20260912 */
    /* AURORA_GPSP_GBA_V14_NATIVE_SQUARE_20260911
     * Interlaced 2x2 handheld presentation leaves physical side bars, so use
     * the existing complete framebuffer clear instead of full-width fast-clear. */
    /* AURORA_GAMBATTE_SQUARE_CLEAR_V13_20260911: retained and extended by GBA V14. */
    GSK_SetGameplayFastClear(
        (!_bMenu && _pSystem && !_MainLoop_BlackScreen &&
         !(GSK_GetActiveVideoMode() != GSK_VIDMODE_240P &&
           bSquareHandheldGameplay)) ? TRUE : FALSE);
    GSK_ResetFrame();

    // render frame
    GPPrimDisableZBuf();

    /* Per-frame full-screen clear to black.
     *
     * MainLoopRender historically NEVER cleared the framebuffer: it
     * relied on the full-screen _OutTex blit below to repaint every
     * pixel.  But that blit is (a) skipped entirely when
     * _MainLoop_BlackScreen is set (boot log + menus) and (b) even when
     * drawn it starts at dy=8, so the top rows are never touched.  With
     * DoubleBuffering=ON each draw goes to the alternate buffer, so any
     * row we don't repaint shows stale content from two frames ago --
     * which appears as a fixed-position horizontal "faixa"/stripe
     * through the text (worst in the log and menus, where nothing
     * covers the background).  Clearing to black first costs a single
     * sprite and removes the band entirely.  GSK_ResetFrame now clears
     * the complete PHYSICAL framebuffer, including any overscan borders.
     * Keep the Poly state reset here, but do not queue a duplicate
     * logical-canvas clear. */
    PolyTexture(NULL);
    PolyBlend(FALSE);
    PolyColor4f(0.0f, 0.0f, 0.0f, 1.0f);
#if AURORA_FRONTEND_PROFILER
    AuroraFrontendProfilerRenderSetupEnd();
    AuroraFrontendProfilerGameDrawBegin();
#endif

	if (!_MainLoop_BlackScreen)
	{
//		Float32 fDestColor = (_bMenu || _MainLoop_ModalCount) ? 0.10f : 0.80f;
		Float32 fDestColor = 0.10f;
		
		if  (!_bMenu && !_MainLoop_ModalCount)
		{
			fDestColor = _MainLoop_fOutputIntensity;
		}

		static Float32 fColor=0.0f;
		Float32 dx = 0.0f;
		Float32 dy = 8.0f;

		if (fColor < fDestColor) 
		{
			fColor+=0.06f;
			if (fColor > fDestColor) 
			{
				fColor = fDestColor;
			}
		} 

		if (fColor > fDestColor) 
		{
			fColor-=0.06f;
			if (fColor < fDestColor) 
			{
				fColor = fDestColor;
			}
		}


        /* AURORA_PD_DIRECT_8BIT_GS_V1_20260821 */
        if (_pSystem == _pNes &&
            QuicknesBridge_CanDirectGsVideo())
        {
            QuicknesBridge_DrawDirectGs(
                _MainLoop_uOutTexTBP,
                g_GskVideoMode == GSK_VIDMODE_240P ? 2 : 4,
                fColor);
        }
        else if (_pSystem == _pFds &&
                 FceummFdsBridge_CanDirectGsVideo())
        {
            /* AURORA_FCEUMM_FDS_PERF_DIRECT_T8_V3_20260827: native FCEUmm XBuf -> GS T8 + CLUT. */
            FceummFdsBridge_DrawDirectGs(
                _MainLoop_uOutTexTBP,
                g_GskVideoMode == GSK_VIDMODE_240P ? 2 : 4,
                fColor);
        }
        else if (!_bMenu && bPceDirectGs)
        {
            /* AURORA_PCE_EXPERIMENTAL_V10_DIRECT_GS */
            PceBridge_DrawDirectGs(
                _MainLoop_uOutTexTBP, fColor);
        }
        else if (bGbaDirectGs)
        {
            /* AURORA_GPSP_GBA_V16_DIRECT_GS_CT16_20260912
             * GPPrimTexRect uses the same transform as PolyRect. Scope the
             * interlaced exact-2x handheld transform around the direct draw,
             * then restore normal UI/status geometry immediately. */
            const Bool bGbaSquareDraw =
                (GSK_GetActiveVideoMode() != GSK_VIDMODE_240P &&
                 bSquareHandheldGameplay) ? TRUE : FALSE;

            if (bGbaSquareDraw)
                GSK_SetGba240pMatchedDraw(1); /* AURORA_GBA_480I_MATCH_240P_ASPECT_V3_2_20261005 */

            _pGba->DrawDirectGs(_MainLoop_uOutTexTBP, fColor);

            if (bGbaSquareDraw)
                GSK_SetGba240pMatchedDraw(0);
        }
        else if (bSegaDirectGs)
        {
            /* AURORA_PD_DIRECT_T8_RENDER */
            PicoDriveBridge_DrawDirectGs(
                _MainLoop_uOutTexTBP, fColor);
        }
        else
        {
            /* AURORA_GPSP_GBA_V14_NATIVE_SQUARE_20260911
             * Scope the 480i/1080i 2x2 transform to the handheld game image;
             * overlays/status/modal geometry immediately returns to normal. */
            /* AURORA_GAMBATTE_DRAW_SCOPE_V13_20260911: retained and extended by GBA V14. */
            const Bool bHandheldSquareDraw =
                (GSK_GetActiveVideoMode() != GSK_VIDMODE_240P &&
                 bSquareHandheldGameplay) ? TRUE : FALSE;
            if (bHandheldSquareDraw)
            {
                if (_pSystem == _pGba)
                    GSK_SetGba240pMatchedDraw(1); /* AURORA_GBA_480I_MATCH_240P_ASPECT_V3_2_20261005 */
                else
                    GSK_SetGbSquarePixelDraw(1);
            }

            PolyBlend(FALSE);
            PolyTexture(&_OutTex);
            PolyUV(0,0,256,240);
    		PolyColor4f(fColor, fColor, fColor, 1.0f);
    
    
                    if (g_GskVideoMode == GSK_VIDMODE_240P &&
                        (_pSystem == _pNes || _pSystem == _pFds)) /* AURORA_FCEUMM_FDS_V0_5_RENDER */
            {
    /*
     * InfoNES 240p overscan compensation.
     *
     * Keep the NES framebuffer at its native 256x240 size and
     * preserve a 1:1 pixel mapping. Only reposition the image
     * to compensate for CRT overscan.
     */
    PolyRect(0.0f, 2.0f, 256.0f, 240.0f);
            }
            else if (g_GskVideoMode == GSK_VIDMODE_240P && _pSystem == _pSega)
            {
    /* AURORA_SEGA_NATIVE_240P_V1
     * PicoDriveBridge already composes MD/SMS/GG into an exact 256x240 logical
     * raster. Present that raster 1:1; PCRTC handles the CRT pixel aspect. */
    PolyRect(0.0f, 0.0f, 256.0f, 240.0f);
            }
            else if (_pSystem == _pSnes)
            {
                /* AURORA_GB_HOTFIX_R13F_STANDALONE_BOOT_AUDIO_Y_20260909_Y_SCOPE:
                 * GB standalone positioning is done in GambatteSystem.
                 * Native SNES/SGB outer presentation remains at Y=8. */
                PolyRect(0.0f, 8.0f, 256.0f, 240.0f);
            }
            else if (_pSystem == _pGba)
            {
                /* AURORA_GPSP_GBA_V14_NATIVE_SQUARE_20260911 */
                PolyRect(0.0f, 0.0f, 256.0f, 240.0f);
            }
            else
            {
    PolyRect(0.0f, 4.0f, 256.0f, 240.0f);
            }
    
            PolyBlend(TRUE);

            if (bHandheldSquareDraw)
            {
                if (_pSystem == _pGba)
                    GSK_SetGba240pMatchedDraw(0);
                else
                    GSK_SetGbSquarePixelDraw(0);
            }
        }

        /* AURORA_QN_LIGHTGUN_OVERLAY_V7_20260829
         * Post-frame overlay: never enters the QuickNES sensor framebuffer. */
        if (_pSystem == _pNes && QuicknesBridge_LightGunActive())
            QuicknesBridge_DrawLightGunCursor(
                g_GskVideoMode == GSK_VIDMODE_240P ? 2 : 4);

        //PolyTexture(NULL);
        //PolyRect(dx,dy,128,120);
    }


#if AURORA_FRONTEND_PROFILER
    AuroraFrontendProfilerGameDrawEnd();
#endif

    #if AURORA_RUNTIME_DIAG_OVERLAY
    /* AURORA_PCE_KRAZY_RUNTIME_DIAG_V11R3_20260830
     * Hidden diagnostics retained for future PCE/QuickNES investigations. */
    if (!_bMenu && _pSystem == _pPce)
    {
        unsigned vw=0, vh=0, pp=0;
        int pfb=0, nativeClass=0;
        int gfb=0, win=0, dw=0, mh=0, sx=0, ovs=0, ws=0;

        PceBridge_GetVideoDebug(&vw, &vh, &pp, &pfb, &nativeClass);
        GSK_GetPceDebugState(&gfb, &win, &dw, &mh, &sx, &ovs, &ws);

        FontSelect(2);
        FontColor4f(1.0f, 1.0f, 0.0f, 1.0f);
        FontPrintf(24, 20, "PCE W%u H%u P%u FB%d N%d", vw, vh, pp, pfb, nativeClass);
        FontPrintf(24, 32, "GFB%d WIN%d DW%d M%d O%d W%d", gfb, win, dw, mh+1, ovs, ws);
    }
    /* AURORA_V6_1G_QN_KRAZY_DEBUG_CONSUMER_CURE_20260831
     * Retired obsolete QuickNES mapper-79 debug overlay; PCE diagnostics above remain. */
    #endif /* AURORA_RUNTIME_DIAG_OVERLAY */

    if (!_bMenu)
    {	
	
#if AURORA_SNES_COST_PROFILER
		if (_pSystem == _pSnes)
			AuroraSnesCostProfilerDrawOverlay();
#endif

		if (s_pMovieClip->IsPlaying())
		{
	        FontSelect(2);
	        FontColor4f(0.5, 0.5f, 0.5f, 1.0f);
	        FontPrintf(240,220, ">");
		}

		if (s_pMovieClip->IsRecording())
		{
	        FontSelect(2);
	        FontColor4f(1.0, 0.0f, 0.0f, 1.0f);
	        FontPrintf(240,220, "O");
		}


		switch (_MainLoop_uDebugDisplay)
        {
		case 0:
/*	        FontSelect(2);
	        FontColor4f(1.0, 1.0f, 1.0f, 1.0f);
	        FontPrintf(40,170, "%08X", InputGetPadData(0));
  */

//		        FontSelect(2);
//		        FontColor4f(1.0, 1.0f, 1.0f, 1.0f);
//		        FontPrintf(40,190, "%3d", xpadGetFrameCount(0,0));
			break;
		case 1:
		/*
	        FontSelect(2);
	        FontColor4f(1.0, 1.0f, 1.0f, 1.0f);
	        FontPrintf(40,190, "%3d %3d", NetInput.InputSize[0], NetInput.OutputSize[0]);
	        FontPrintf(40,200, "%3d %3d", NetInput.InputSize[1], NetInput.OutputSize[1]);
	        FontPrintf(40,210, "%3d %3d", NetInput.InputSize[2], NetInput.OutputSize[2]);
	        FontPrintf(40,220, "%3d %3d", NetInput.InputSize[3], NetInput.OutputSize[3]);
			*/
			break;
		case 2:
	        FontSelect(2);
	        FontColor4f(1.0, 1.0f, 1.0f, 1.0f);
	        FontPrintf(40,170, "%08X", _uInputFrame);
	        FontPrintf(40,180, "%08X", _uInputChecksum[0]);
	        FontPrintf(40,190, "%08X", _uInputChecksum[1]);
	        FontPrintf(40,200, "%08X", _uInputChecksum[2]);
	        FontPrintf(40,210, "%08X", _uInputChecksum[3]);
	        FontPrintf(40,220, "%08X", _uInputChecksum[4]);
			break;
		case 3:
			FontColor4f(1.0, 0.0f, 0.0f, 1.0f);
			FontPrintf(195, 210, "%8d", _uVblankCycle / 1024);
			break;
        }

        FontSelect(2);
		FontColor4f(1.0, 1.0f, 1.0f, 1.0f);
		{

/*
		FontPrintf(15, 180, "%08X %08X Y", (Int32)(_ColorCalib.y_mul * 0x10000), (Int32)(_ColorCalib.y_add * 0x10000));
		FontPrintf(15, 190, "%08X %08X I", (Int32)(_ColorCalib.i_mul * 0x10000), (Int32)(_ColorCalib.i_add * 0x10000));
		FontPrintf(15, 200, "%08X %08X Q", (Int32)(_ColorCalib.q_mul * 0x10000), (Int32)(_ColorCalib.q_add * 0x10000));
  */

		/*
		FontPrintf(195, 180, "%6.3f %6.3f", _ColorCalib.y_mul, _ColorCalib.y_add);
		FontPrintf(195, 190, "%6.3f %6.3f", _ColorCalib.i_mul, _ColorCalib.i_add);
		FontPrintf(195, 200, "%6.3f %6.3f", _ColorCalib.q_mul, _ColorCalib.q_add);
		*/
		}

//			FontPrintf(195, 210, "%8d", _AudMix.GetLastOutput());
    }


#if AURORA_FRONTEND_PROFILER
    AuroraFrontendProfilerUiBegin();
#endif
	/* Keep menu audio alive even while a modal overlays the UI. Previously
	   BgmUpdate lived only in the non-modal branch below, so every fixed-time
	   message starved audsrv regardless of whether any I/O was happening. */
	if (_bMenu)
	{
		/* AURORA_FINAL_V1_3_NORMAL_MENU_BGM_SESSION_20260901
		 * _bMenu alone is insufficient: isolated quick-state/device/format
		 * prompts also pause the core without acquiring the CD transport.
		 * BGM may scan/open storage, so it is legal only for a session that
		 * successfully entered through _MenuEnable(TRUE). */
		if (MainLoopNormalMenuBgmSessionActive() &&
		    MainLoopCdUiReady() && /* AURORA_SSF2_PCE_MENU_FIX_V2_20260913_PCE_MENU_ASYNC */
		    _MainLoop_pScreen != (CScreen *)_MainLoop_pStateConfirmScreen)
			BgmUpdate();
		/* Draw the live menu first, then place modal/status text on top. The
		   previous order painted _MenuDraw after the status and hid it. */
		_MenuDraw();
	}

	if (_MainLoop_ModalCount > 0)
	{
		FontSelect(0);
		{
			const Int32 textW = FontGetStrWidth(_MainLoop_ModalStr);
			const Int32 textX = 128 - textW / 2;
			const Bool bErrorModal =
				(!strncmp(_MainLoop_ModalStr, "ERROR:", 6) || !strncmp(_MainLoop_ModalStr, "Missing INES mapper", 19)) ? TRUE : FALSE;

			/* AURORA_TSUKURU_8M_GBA_LOAD_AUDIO_REDERR_V1_20260913_RED_ERRORS
			 * Loader failures can leave the previous framebuffer white, black or
			 * partially rendered. ERROR: is already Aurora's modal error contract;
			 * erase that accidental background and give every error the same
			 * opaque red screen. Non-error informational modals retain the old
			 * compact black backing.
			 */
			PolyTexture(NULL);
			PolyBlend(FALSE);
			if (bErrorModal)
			{
				PolyColor4f(0.55f, 0.0f, 0.0f, 1.0f);
				PolyRect(0.0f, 0.0f,
				         (Float32)MAINLOOP_SCREENWIDTH,
				         (Float32)MAINLOOP_SCREENHEIGHT);
				/* Darker red text plate: readable but still visibly an error. */
				PolyColor4f(0.30f, 0.0f, 0.0f, 1.0f);
			}
			else
			{
				PolyColor4f(0.0f, 0.0f, 0.0f, 1.0f);
			}
			PolyRect((Float32)(textX - 4), 96.0f,
			         (Float32)(textW + 8), 16.0f);
			PolyBlend(TRUE);

			FontColor4f(1.0, 1.0f, 1.0f, 1.0f);
			FontPrintf(textX, 100, _MainLoop_ModalStr);
		}

		_MainLoop_ModalCount--;
	}
	else
	{
		if (_MainLoop_StatusCount > 0)
		{
			FontSelect(0);
			/* AURORA_V15_STATUS_SHADOW_REVERT_20260824
			 * Revert V15 black double-draw: transient status text is plain cyan again. */
			FontColor4f(0.0, 0.8f, 0.8f, 1.0f);
			FontPrintf(20, 200, _MainLoop_StatusStr);
			_MainLoop_StatusCount--;
		}
	}

#if AURORA_FRONTEND_PROFILER
    AuroraFrontendProfilerUiEnd();
#endif
	#if CODE_DEBUG
	if (_MainLoop_bMCSaveReady && MCSave_WriteSync(FALSE, NULL))
	{
		FontSelect(1);
		FontColor4f(1.0, 0.0f, 0.0f, 1.0f);
		if (_iFrame & 4)
			FontPrintf(235,216, "#");
	}
	#endif



#if AURORA_FRONTEND_PROFILER
    AuroraFrontendProfilerGpFlushBegin();
#endif
    PROF_ENTER("GPFlush");
#if AURORA_C4_PROFILER
    const Uint32 c4Flush=g_AuroraC4ProfilerActive?ProfCtrGetCycle():0u;
#endif
    GPFifoFlush();
#if AURORA_C4_PROFILER
    if(g_AuroraC4ProfilerActive)AuroraC4ProfilerGpFlush((Uint32)(ProfCtrGetCycle()-c4Flush));
#endif
    PROF_LEAVE("GPFlush");
#if AURORA_FRONTEND_PROFILER
    AuroraFrontendProfilerGpFlushEnd();
#endif

    /* gsKit_sync_flip waits for vsync, swaps the display buffer
       and resets gsKit's draw queue for the next frame. The
       legacy WaitForNextVRstart / GS_SetCrtFB / GS_SetDrawFB
       block is now subsumed by this single call. */
    /* V4: end recovery work timing before the blocking VSync wait. */
    s_SafeFrameskipPreFlip =
        (s_SafeFrameskipTickStart != 0u) ? ProfCtrGetCycle() : 0u;

    PROF_ENTER("WaitVBlank");
#if AURORA_C4_PROFILER
    const Uint32 c4Vb=g_AuroraC4ProfilerActive?ProfCtrGetCycle():0u;
#endif
    if ( (_iFrame&15)==0)   _uVblankCycle = ProfCtrGetCycle();
#if AURORA_FRONTEND_PROFILER
    AuroraFrontendProfilerVBlankBegin();
#endif
    GSK_SyncFlip();
#if AURORA_FRONTEND_PROFILER
    AuroraFrontendProfilerVBlankEnd();
#endif
    if ( (_iFrame&15)==0)   _uVblankCycle = ProfCtrGetCycle() - _uVblankCycle;
#if AURORA_C4_PROFILER
    if(g_AuroraC4ProfilerActive)AuroraC4ProfilerVBlank((Uint32)(ProfCtrGetCycle()-c4Vb));
#endif
    _MainLoopSafeFrameskipAfterFlip();
    PROF_LEAVE("WaitVBlank");

    /* AURORA_AUDIO_FAILSOFT_POSTVBLANK_V1
     * The frame is already presented. Keep the normal gameplay audio drain
     * here so synchronous audsrv RPC latency is moved out of the pre-render
     * deadline. Aud_BufferedAsyncStart uses only wait=0 drains. */
    if (!_bMenu && _pSystem && !_MainLoop_BlackScreen)
    {
#if AURORA_C4_PROFILER
        const Uint32 c4PostAud=g_AuroraC4ProfilerActive?ProfCtrGetCycle():0u;
#endif
#if AURORA_FRONTEND_PROFILER
        AuroraFrontendProfilerAudioDrainBegin();
#endif
        Aud_BufferedAsyncStart();
#if AURORA_FRONTEND_PROFILER
        AuroraFrontendProfilerAudioDrainEnd();
#endif
#if AURORA_C4_PROFILER
        if(g_AuroraC4ProfilerActive)AuroraC4ProfilerPostAudio((Uint32)(ProfCtrGetCycle()-c4PostAud),FALSE);
#endif

        /* V15 completed presented-host-tick boundary. Commit AFTER the
         * post-VBlank drain because that work occurs before the next Take()
         * during uninterrupted gameplay and therefore belongs to phase. */
        _MainLoopSafeFrameskipCommitGameplayBoundary(
            ProfCtrGetCycle());
    }

#if AURORA_FRONTEND_PROFILER
    AuroraFrontendProfilerRenderEnd();
#endif
    _iFrame++;
}


/* AURORA_PCE_KRAZY_RUNTIME_DIAG_V11R3_20260830 */

/* AURORA_PCE_ROOT512_KRAZY_LATCH_V12_20260830 */

/* AURORA_PCE_RELEASE_CLEANUP_FINAL_V15_20260830 */
