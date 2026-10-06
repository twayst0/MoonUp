# MoonUp (Türkçe)

> [English README](../README.md)

MoonUp, bir oyun penceresini gerçek zamanlı olarak yakalayıp daha yüksek çözünürlükte yeniden oluşturan, ara kareler üreten ve görüntüyü nöral bir katmanla iyileştiren bir Windows uygulamasıdır. Lossless Scaling ile aynı mantıkla çalışır: oyuna hiçbir şey enjekte edilmez, tüm işlem ekran kartında DirectX 11 compute shader'larıyla yapılır. Bu yüzden NVIDIA, AMD ve Intel kartlarda aynı şekilde çalışır.

## Kullanım

1. `MoonUp.exe`'yi aç. İlk açılışta dil seçimi ve donanıma göre otomatik ayar yapılır.
2. Oyununu **pencereli** ya da **kenarlıksız (borderless)** modda çalıştır. Özel tam ekran (exclusive fullscreen) yakalanamaz.
3. Kontrol ekranında hedefi seç ve **Ölçekle**'ye bas (ya da oyundayken `Ctrl + Alt + S`).
4. Durdurmak için aynı kısayola tekrar bas.

## Klasör içeriği

| Dosya | Açıklama |
|---|---|
| `MoonUp.exe` | Uygulama (arayüz + ayrı süreçte çalışan motor) |
| `WebView2Loader.dll` | Microsoft WebView2 yükleyicisi (arayüz için) |
| `ui\` | Arayüz dosyaları (HTML/CSS/JS, yazı tipleri, dil dosyaları) |

Ayarlar `%APPDATA%\MoonUp\settings.json` içinde, kayıtlar programın yanındaki `logs\` klasöründe tutulur:

- `moonup.log`: arayüz
- `moonup-engine.log`: ölçekleme motoru (yakalama, işleme, çıkış)
- `engine-crash.dmp` / `crash.dmp`: bir çökme olursa

Motor ayrı bir süreçte çalışır. Ekran kartı sürücüsü ya da yakalama tarafında bir sorun olursa yalnızca motor durur; MoonUp açık kalır, hatayı gösterir ve yeniden başlatabilirsin.

## Hazır profiller

İlk açılışta Profiller sayfasına beş hazır profil eklenir (kendi profillerine dokunulmaz):

| Profil | Ölçekleyici | Kare üretimi | Neural Render | Ne için |
|---|---|---|---|---|
| Dengeli | AMD FSR 1 | 2× (dengeli) | kapalı | Lossless Scaling benzeri temel ayar |
| Nöral kalite | MoonUp Neural | 2× (kalite) | kapalı | En iyi ölçekleme kalitesi |
| Nöral Render | AMD FSR 1 | 2× (dengeli) | açık (ton 1.0, yapı 1.0) | DLSS 5 tarzı görünüm |
| Performans | NVIDIA NIS | 3× (performans) | kapalı | Zayıf ekran kartı / yüksek FPS |
| Maksimum | MoonUp Neural | uyarlanabilir (kalite) | açık (ton 1.5, yapı 1.5) | Her şey açık, en ağır yük |

## Teknoloji

- **MoonUp Motion (kare üretimi)**: Parlaklık piramidi üzerinde piksel başına yoğun optik akış. Her seviyede önceki kare çiftinin hareketinden tahmin (zamansal aday), komşulardan yayılım (PatchMatch tarzı) ve çeyrek piksele kadar iyileştirme yapılır. Ara karede her 2×2 blok için birden çok hareket adayı iki kareyle birlikte sınanır, en iyi açıklayan seçilir ve alt piksel hassasiyetinde düzeltilir. Birkaç kare boyunca hiç değişmeyen pikseller (yazılar, nişangâh, arayüz) "HUD kilidi" ile asla kaydırılmaz. Sahne değişimi algılanınca ara kare üretilmez.
- **MoonUp Neural Render v3**: NVIDIA'nın DLSS 5 için yayımladığı tasarımı örnek alan tek adımlı, deterministik nöral görüntüleme katmanı (bir kare girer, bir kare çıkar; ton ve yapı yoğunluğu ayrı).
  - *Ton* (düşük frekanslı ışık ve renk): 64 kanal genişliğinde evrişimli bir ağ karenin 256×144 kopyasını ve genel ışık bağlamını okur, 16×9 hücre × 8 parlaklık katmanlı bir iki taraflı (bilateral) renk dönüşümü ızgarası tahmin eder; her piksel dönüşümünü öğrenilmiş bir kılavuzla seçer.
  - *Yapı* (yüksek frekanslı gölgeleme): yarım çözünürlüklü parlaklık üzerinde 12 kanallı, genişletilmiş (dilated, 1-2-4-8) bir ağ, 71 piksellik bağlamla temas gölgesi / ortam örtülmesi (log gölgeleme) ve detay kazancı tahmin eder.
  - Izgara ve yapı haritaları zaman içinde taşınır ve yalnızca görüntünün değiştiği yerde güncellenir. Toplam 154.562 öğrenilmiş parametre; yaklaşık 790 fotoğrafla eğitildi. Ton, malzeme/renk, yapı ve zamansal kararlılık ayrı ayrı ayarlanır.
- **AMD FSR 1**: FidelityFX Super Resolution 1 (EASU ölçekleme + RCAS keskinleştirme), referans koddan birebir HLSL'e taşındı (MIT).
- **NVIDIA NIS**: NVIDIA Image Scaling v1.0.3 (6 dokunuşlu yönsel ölçekleme + uyarlanabilir keskinleştirme), resmi shader kodu (MIT). 2×'e kadar çalışır; daha büyük ölçeklerde otomatik olarak FSR devralır.
- **MoonUp Edge**: Yapı tensörüyle kenar yönünü bulan, kenar boyunca uzayan ve kenara dik daralan anizotropik bir Lanczos çekirdeği. Halkalanma yerel aralığa sıkıştırılır.
- **MoonUp Neural v3 (ölçekleyici)**: ArtCNN C4F16 (MIT) ağırlıklarından başlatılıp fotoğraflar, oyun benzeri sahneler ve arayüz/yazı görüntüleriyle, oyunların düşük çözünürlükte çizilme biçimine benzeyen bozulmalarla (alan, bikübik, nokta örnekleme, hafif yumuşaklık) ince ayar yapılmış 16 kanallı, 7 katmanlı ağ; 2× parlaklığı doğrudan tahmin eder. Ayrılmış test görüntülerinde ArtCNN'den +0,12 dB, Lanczos'tan +0,7–1 dB daha iyi; v2'deki aşırı keskinleştirme haleleri yok. (`tools/train_neural_ft.py`)
- **ArtCNN (anime / 2D)**: ArtCNN C4F16'nın kendi eğitilmiş ağırlıkları (MIT), aynı shader'larla çalışır. Anime ve 2D çizim tarzı oyunlar için.
- **MoonUp Vision**: Yerel kontrast, ince ayrıntı, canlılık, ton ve beyaz dengesi.
- **Keskinleştirme**: Kontrasta uyumlu keskinleştirme (AMD FidelityFX CAS fikri, MIT) ve düz alanlarda gürültü koruması.
- **Yakalama**: Windows Graphics Capture (Windows 11 24H2'de varsayılan `MinUpdateInterval` yakalamayı ~50 FPS'e kıstığı için 1 ms'ye ayarlanır); olmazsa DXGI Desktop Duplication; o da olmazsa GDI (son çare).
- **Çıkış**: Flip model swap chain ve bekleme nesnesi. Desteklenmeyen sistemlerde adım adım daha basit yapılandırmalara düşer.

## MoonUp Upgraph (oyun içi nöral render + FSR 3.1)

Sol menüde **Upgraph** sayfası. Ekran yakalamak yerine efekti **oyunun içine** kurar; bu yüzden yakalama/overlay kaynaklı FPS kaybı olmaz ve oyunun **derinlik bilgisi** kullanılabilir.

1. Oyunlar otomatik bulunur (Steam, Epic, GOG, Ubisoft Connect). Bulunmayanlar için klasör ekle düğmesi var.
2. Oyunu seç: exe, grafik API'si (DX9/10/11/12, OpenGL, Vulkan), 32/64-bit, oyundaki DLSS/FSR/XeSS dosyaları ve anti-cheat otomatik tespit edilir.
3. **MoonUp Upgraph (nöral render)**: ReShade (açık kaynak, reshade.me'den otomatik indirilir; indirilemezse ReShade_Setup.exe dosyasını kendin seçebilirsin) + `MoonUpUpgraph.fx` kurulur. İçinde masaüstü motordaki Nöral Render ağının birebir aynısı var (Wine testinde masaüstü motorla fark: ortalama 0/255), ayrıca oyunun derinlik bilgisinden **temas gölgeleri** ve AMD **RCAS** keskinlik.
4. **AMD FSR 3.1 + kare üretimi (OptiScaler)**: oyunda DLSS, FSR 2+ veya XeSS varsa OptiScaler'ın son sürümü GitHub'dan indirilir; oyunun upscaler'ı FSR 3.1 ile, kare üretimi FSR frame generation (OptiFG) ile yapılır. AMD kartlarda da çalışır. Oyunda DLSS/FSR/XeSS seçeneğini açman gerekir.
5. Değiştirilen her dosya önce `_MoonUp_Upgraph\backup` içine yedeklenir; **Kaldır** her şeyi eski haline getirir.

Oyunda tuşlar: **Home** = ReShade menüsü (MoonUp Upgraph ayarları), **Scroll Lock** = efekti aç/kapat (önce/sonra), **Insert** = OptiScaler menüsü. İlk açılışta efekt bir kez derlenir (10–30 sn), sonra önbellekten açılır.

İndirilenler `MoonUp\_cache\upgraph` klasörüne (MoonUp'ın yanına, yani Z: diskine) kaydedilir, C: diskine değil.

Uyarı: anti-cheat'li online oyunlarda kullanma (ReShade/OptiScaler ban sebebi olabilir). Vulkan oyunlarında nöral efekt için ReShade'in kendi kurulumu gerekir; FSR 3.1 kısmı Vulkan'da da çalışır.

### DLSS5-Swapper incelemesi

https://github.com/rakanki911/DLSS5-Swapper tamamen incelendi. Bu program **DLSS 5'in kendisini içermez**: NVIDIA'nın kapalı `nvngx_dlssnr.dll` dosyasını (yalnızca GeForce RTX 50 / Blackwell'de çalışır) ReShade eklentileri (DLSS5-Feeder, RenoDX) ve OptiScaler üzerinden oyunlara kuran bir yöneticidir. Açık kaynak bir DLSS 5 modeli yok. Upgraph aynı mantığı (oyun kütüphanesi tarama, API tespiti, ReShade ile oyuna kurma, yedekleme/geri yükleme, oyun içi menüden ayar, OptiScaler) uygular; ama NVIDIA'nın kapalı modeli yerine MoonUp'ın kendi açık nöral render ağını kullanır, bu yüzden AMD RX 6700 XT dahil her ekran kartında çalışır.


### NVIDIA DLSS 5 seçeneği (yalnızca NVIDIA RTX kartlarda görünür)

Upgraph sayfasında, bilgisayarda NVIDIA GeForce RTX kart varsa **NVIDIA DLSS 5 (nöral render)** seçeneği çıkar (AMD/Intel kartlarda gizlidir). DLSS5-Swapper'ın kurulum mantığı birebir uygulandı; hiçbir dosya MoonUp ile dağıtılmaz, hepsi kurulumda projelerin kendi GitHub sürümlerinden indirilir:

- **ReShade eklenti (Addon) sürümü** (reshade.me) — DLSS 5 bileşenleri ReShade eklentisi olarak çalışır.
- **RenoDX DLSS 5 eklentisi** ve NVIDIA **DLSS 5 modeli** (`nvngx_dlssnr.dll`) + gerekirse **DLSS çalışma dosyası** (`nvngx_dlss.dll`) — RHI sürüm deposundan (github.com/RankFTW/rhi-repo), her seferinde en yeni sürüm.
- **DLSS5-Feeder** (MIT, github.com/jlrouzies-fr/DLSS5-Feeder) + ReShade başlık dosyaları + **LumeniteFX** (hareket vektörleri) — oyunda DLSS yoksa.
- **dgVoodoo2** — DirectX 8/9 oyunlarında (oyunu DirectX 11'e çevirir).

MoonUp yolu otomatik seçer: 64-bit DirectX 10/11/12 oyunda **kendi DLSS'i varsa** RenoDX oyunun DLSS çağrılarına bağlanır (en iyi kalite); **yoksa** DLSS5-Feeder DLSS çağrısını ReShade'in karesinden kendisi yapar, yani DLSS'i olmayan oyunlara da DLSS 5 gelir. 32-bit oyunlarda 64-bit yardımcı (host64), DirectX 8/9'da dgVoodoo2 kullanılır. Vulkan oyunları için DLSS5-Feeder'ın kendi kurulumu gerekir. OpenGL desteklenir.

Kart desteği: NVIDIA'nın kendi DLSS 5 modeli **RTX 50** kartlarda çalışır. MoonUp (DLSS5-Swapper'ın ReShade/Feeder yolları gibi) RHI deposundaki **değiştirilmiş DLSS 5 çalışma dosyasını** (310.8.Lecram) kurar; yapımcısı bu dosyanın **RTX 20/30/40**'ta da çalıştığını bildiriyor, bu yüzden bu kartlarda seçenek "deneysel" etiketiyle açıktır (garanti değil). Kartına özel başka bir `nvngx_dlssnr.dll` varsa "Kendi DLSS 5 modelin" ile gösterilebilir, o kullanılır.

Diğer güvenlik adımları (Swapper'daki gibi): ReShade'in uyumluluk listesinde ReShade'i yasaklayan oyunlar reddedilir, başka nöral eklentiler / OptiScaler kenara alınır (yedeklenir), eski `d3dcompiler_47.dll` yedeklenir, anti-cheat uyarısı verilir. DLSS 5 ile OptiScaler aynı anda kurulamaz (ikisi de oyunun DLSS çağrısını alır). Kaldır her şeyi geri getirir.

Oyunda: oyunun ayarlarından DLSS'i aç (kendi DLSS'i olan oyunlarda), **Home > Add-ons > RenoDX** menüsünde DLSS 5 ayarları vardır.

Not: Bu kısım AMD kartlı geliştirme bilgisayarında gerçek oyunda denenemedi; kurulum/kaldırma Wine'da sahte oyunla test edildi. RTX 50 sahipleri test etmeli.

## Performans koruması (FPS düşmesin diye)

Önce FPS, sonra kalite. MoonUp kendi GPU payını **%30'un altında** tutar ve gösterilen FPS'i oyunun FPS'inin altına düşürmemeye çalışır:

- **Upscaling tek başına FPS artırmaz.** FPS'i artıran şey oyunun daha az piksel çizmesidir. Bunun için **Ölçekleme → Oyun çözünürlüğü** (%85 / %75 / %67 / %50) seçeneğini kullan: MoonUp oyunun penceresini küçültür, oyun daha hızlı çizer, MoonUp görüntüyü tekrar tam ekrana büyütür. Durdurunca pencere eski haline döner. Pencereli ve kenarlıksız oyunlarda çalışır. Exclusive (özel) tam ekranda çalışmaz. Bazı oyunlar pencereyi kendi boyutuna geri çekebilir, o zaman oyunun içinden pencereli moda alıp çözünürlüğü elle düşür.
- Ekran yenileme hızından fazla kare **işlenmez**, çünkü gösterilemez (ör. 240 FPS oyun ve 144 Hz ekranda en fazla ~150 kare işlenir).
- Yapılacak bir şey yoksa (ölçekleme, kare üretimi ya da efekt yoksa) overlay hiç açılmaz ve oyun doğrudan, tam hızda gösterilir (passthrough).
- GPU payı aşılırsa ya da oyun tek başına çalıştığından %20+ yavaşlarsa sırayla şunlar devreye girer: hafif hareket araması → hafif upscaler (Neural/ArtCNN → FSR, Lanczos → Edge) → Neural Render / Vision kapalı → kare üretimi kapalı → işleme hızı sınırı → en son çare bypass (oyun doğrudan gösterilir). Kare hızını sınırlamak artık en son adım. Bir süre rahat çalışınca bir kademe geri çıkar.
- Oyun tek başına ekranı dolduruyorsa (başlangıç ölçümünde oyun FPS ≥ ekran yenileme × 0,9) kare üretimi hiç başlatılmaz. Ölçek de yoksa oyun doğrudan gösterilir.
- **Çökme koruması:** Overlay açılınca oyun kendi hızının %30'unun altına düşerse (ör. 400 → 10 FPS), bu kalite düşürerek düzelmez. O zaman kare üretimi ve efektler hemen kapanır, yetmezse yarım saniye içinde oyun doğrudan gösterilir. 20 sn sonra bir kez daha denenir. Yine çökerse o oturum boyunca oyun doğrudan gösterilir ve bir uyarı çıkar. Log'da `Perf: ... collapse` satırı yazılır.
- GPU süresi ölçümü yüksek FPS'te kaybolan ölçümleri artık tahminle tamamlıyor (16 sorgu yuvası, iş sayısı × ortalama süre). Eskiden 400 FPS'lik oyunlarda GPU payı olduğundan çok düşük görünüyordu.
- Başlarken oyunun örtülmemiş FPS'i 0,6 sn ölçülür. Overlay oyunu örttüğünde oyun yavaşlarsa overlay otomatik olarak "tam örtmeyen" moda geçer.
- HUD'da ipucu çıkar ("Auto: ..."). Log'da `Perf:`, `Passthrough:`, `Render scale:` ve `Stats:` satırları yazılır.

### FPS sayacı

Ayarlar → Katman → **FPS sayacı** açıksa sayaç her zaman görünür: MoonUp ölçekleme yaparken kendi HUD'unda, ölçekleme yapmıyorken (ya da passthrough/bypass sırasında) önündeki pencerenin köşesinde küçük bir sayaç olarak. Sayaç tıklamaları engellemez. Değerler: FPS (60+ beyaz, 30–60 sarı, 30 altı kırmızı) ve kare süresi (ms). Sayaç pencerenin sunduğu kareleri Windows Graphics Capture ile sayar ve kopyalamaz, yani maliyeti çok düşüktür.
- Windows 10'da yakalama sarı çerçeve gösterdiği için ölçekleme dışındaki pencerelerde sayaç gösterilmez, yalnızca ölçeklenen oyunda gösterilir.
- Exclusive tam ekran oyunlarda (pencere üstü katman görünmez) sayaç görünmez. Oyunu kenarlıksız ya da pencereli moda al.

## DLSS hakkında dürüst not

NVIDIA DLSS kapalı bir teknolojidir; GeForce RTX tensor çekirdeklerinde çalışır ve her oyuna ayrıca eklenmesi gerekir. DLSS 5, oyunun kendi verilerini (hareket vektörleri ve diğer render bilgileri) kullanan ve RTX 50 serisinde çalışan büyük, tek adımlı bir difüzyon modelidir. Açık kaynak bir DLSS 5 yoktur.

MoonUp DLSS içermez ve onu taklit etmez. MoonUp Neural Render aynı tasarım fikrini (tek adım, deterministik, zamansal durum taşıyan, sanatsal kontrolleri olan nöral görüntüleme) tüm ekran kartlarında çalışacak şekilde uygular. Ancak yalnızca bitmiş kareyi gördüğü için yeni ışık icat edemez; karede zaten var olan ışığı, gölgelemeyi ve renkleri güçlendirir. Oyununda DLSS/FSR/XeSS varsa oyunun kendi ölçekleyicisini açıp MoonUp'ın kare üretimini ve Neural Render'ını üstüne kullanabilirsin.

AMD FSR 4'ün internete sızan kaynak kodu lisanssız olduğu için kullanılmadı. MoonUp'a yalnızca açık lisanslı (MIT) kodlar alındı: FSR 1 ve NVIDIA Image Scaling.

## Kaynaktan derleme

Linux üzerinde mingw-w64 ile çapraz derlenir:

```
apt install g++-mingw-w64-x86-64-posix glslang-tools
tools/check_shaders.sh   # HLSL doğrulama
tools/build.sh           # build/MoonUp.exe
```

Shader'lar derleme sırasında Microsoft'un `d3dcompiler_47.dll` derleyicisiyle (Wine altında) önceden derlenir ve DXBC olarak exe'nin içine gömülür (`tools/build_shaders.sh` → `src/engine/shaders_bin.h`). Kullanıcının bilgisayarında hiçbir shader derlenmez; motor anında başlar. Kaynak değişmişse (kaynak özeti tutmazsa) çalışma anında derleme yedek yol olarak kalır.

```
tools/build_shaders.sh   # shader değiştiğinde
tools/build.sh
```

### Test düzeneği

`tools/bench/` gerçek motor kodunu Wine altında (Mesa llvmpipe üzerinde) çalıştırır:

- `make_scenes.py`: kesin ara kare karşılıkları olan sentetik oyun sahneleri üretir.
- `fg_eval.sh` ve `score.py`: kare üretimini gerçek ara karelere karşı ölçer (PSNR ve en kötü %2 blok).
- `bench.exe engine` ve `bench.exe hosttest`: yakalama, işleme, çıkış ve ayrı motor sürecini uçtan uca çalıştırır.

### Ağların eğitimi

- `tools/train_neural.py`: MoonUp Neural ölçekleyici (ArtCNN ağırlıklarını da ONNX dosyasından aktarır).
- `tools/train_render.py`: MoonUp Neural Render (JAX). `tools/render_ref.py` GPU sonucunu doğrulayan NumPy referansıdır.

## Üçüncü taraf bileşenler

- Microsoft Edge WebView2 (çalışma zamanı ve yükleyici): Microsoft
- nlohmann/json: MIT
- Inter ve JetBrains Mono yazı tipleri: SIL Open Font License 1.1 (`ui/fonts`)
- Kontrasta uyumlu keskinleştirme fikri: AMD FidelityFX CAS, MIT
- AMD FidelityFX Super Resolution 1 (EASU + RCAS, `src/engine/shaders/fsr.hlsl`): MIT, © 2021 Advanced Micro Devices, Inc. — https://github.com/GPUOpen-Effects/FidelityFX-FSR
- NVIDIA Image Scaling SDK v1.0.3 (`src/engine/shaders/nis.hlsl`, `src/third_party/nis/NIS_Config.h`): MIT, © 2022 NVIDIA CORPORATION & AFFILIATES — https://github.com/NVIDIAGameWorks/NVIDIAImageScaling
- Kare üretimi tasarımında incelenen açık kaynak: AMD FidelityFX SDK (FSR 3 kare enterpolasyonu ve optik akış), MIT
- Neural Render mimarisi: Gharbi ve ark., "Deep Bilateral Learning for Real-Time Image Enhancement" (2017); Yu ve Koltun, "Multi-Scale Context Aggregation by Dilated Convolutions" (2016); Chen ve ark., "Fast Image Processing with Fully-Convolutional Networks" (2017); tasarım hedefleri NVIDIA'nın DLSS 5 hakkında yayımladığı bilgilerden (SIGGRAPH 2026)
- ArtCNN C4F16 ağırlıkları ve mimarisi (`src/engine/neural_weights.h` içindeki `kArtCNN`): MIT, © 2024 João Chrisóstomo — https://github.com/Artoriuz/ArtCNN
- ReShade (Upgraph tarafından kullanıcının bilgisayarına indirilir): BSD 3-Clause, © 2014 Patrick Mours — https://github.com/crosire/reshade. Test aracında ReShade FX derleyicisi kullanıldı (`tools/third_party/reshadefx`, BSD 3-Clause).
- OptiScaler (isteğe bağlı, Upgraph tarafından GitHub'dan indirilir, MoonUp ile dağıtılmaz): GPL-3.0 — https://github.com/optiscaler/OptiScaler
- Upgraph tasarımı ve DLSS 5 kurulum yolu DLSS5-Swapper'dan (MIT, Rakan Alkhaldi) uyarlandı. DLSS 5 için indirilenler (kullanıcının bilgisayarına, MoonUp ile dağıtılmaz): DLSS5-Feeder (MIT), RenoDX DLSS 5 eklentisi (ShortFuse, RHI deposu), NVIDIA DLSS/DLSS 5 çalışma dosyaları (NVIDIA lisansı), LumeniteFX, dgVoodoo2 (kendi lisansı), ReShade Addon (BSD-3). miniz (MIT) zip açmak için kullanılır.
- Eğitim verisi: Kodak Lossless True Color Image Suite, Urban100, BSDS500 (Berkeley), General100 + T91 (igv/FSRCNN-TensorFlow deposundan, MIT), scikit-image örnek görüntüleri, MoonUp'ın sentetik oyun sahneleri ve arayüz görüntüleri (yalnızca eğitimde kullanıldı, dağıtılmaz)
