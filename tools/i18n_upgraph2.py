#!/usr/bin/env python3
"""Upgraph progress, error and DLSS 5 strings for de/es/fr/it/pt/ru/ja/zh (idempotent)."""
import json, os, re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KEYS = ["upgraph.active", "upgraph.stage.start", "upgraph.stage.reshade_find", "upgraph.stage.reshade_download", "upgraph.stage.reshade_extract",
        "upgraph.stage.optiscaler_find", "upgraph.stage.optiscaler_download", "upgraph.stage.optiscaler_extract", "upgraph.stage.install",
        "upgraph.stage.done", "upgraph.stage.remove", "upgraph.err.reshade", "upgraph.err.optiscaler_download", "upgraph.err.extract",
        "upgraph.err.disk", "upgraph.err.anticheat", "upgraph.err.no_exe", "upgraph.err.vulkan", "upgraph.err.api", "upgraph.err.opti_32bit",
        "upgraph.err.opti_api", "upgraph.err.not_installed", "upgraph.err.not_found", "upgraph.dlss5", "upgraph.dlss5.native",
        "upgraph.dlss5.feeder", "upgraph.dlss5.needModel", "upgraph.dlss5.vulkan", "upgraph.dlss5.api", "upgraph.dlss5.model",
        "upgraph.dlss5.model.d", "upgraph.dlss5.modelReady", "upgraph.dlss5.pick", "upgraph.dlss5.force", "upgraph.dlss5.force.d",
        "upgraph.keysDlss5", "upgraph.keysDlss5Feeder", "upgraph.stage.dlss5_find", "upgraph.stage.dlss5_model", "upgraph.stage.dlss5_consumer",
        "upgraph.stage.dlss5_runtime", "upgraph.stage.dlss5_feeder", "upgraph.stage.dlss5_motion", "upgraph.stage.dlss5_dgvoodoo",
        "upgraph.err.dlss5_opti", "upgraph.err.dlss5_gpu", "upgraph.err.dlss5_custom", "upgraph.err.dlss5_vulkan", "upgraph.err.dlss5_download",
        "upgraph.err.dlss5_file", "upgraph.err.reshade_banned"]

T = {
"de": ["Upgraph", "Vorbereitung", "Suche ReShade", "Lade ReShade", "Entpacke ReShade", "Suche OptiScaler", "Lade OptiScaler", "Entpacke OptiScaler", "Installiere", "Fertig", "Entferne",
       "Diese Datei ist kein ReShade-Setup.", "OptiScaler konnte nicht von GitHub geladen werden.", "Der Download konnte nicht entpackt werden (.7z von OptiScaler braucht Windows 11 23H2 oder neuer).",
       "Eine Datei konnte nicht geschrieben werden. Ist der Spieleordner schreibgeschützt oder die Festplatte voll?", "Dieses Spiel nutzt Anti-Cheat; bestätige zuerst die Warnung.", "Keine Spiel-EXE gefunden.",
       "Vulkan-Spiele brauchen den ReShade-Installer selbst.", "Diese Grafik-API wird nicht unterstützt.", "OptiScaler unterstützt nur 64-Bit-Spiele.", "OptiScaler unterstützt DirectX-11/12- und Vulkan-Spiele.",
       "Upgraph ist in diesem Spiel nicht installiert.", "Der Spieleordner existiert nicht mehr.", "NVIDIA DLSS 5 (Neural Rendering)",
       "Dieses Spiel hat DLSS: Das RenoDX-DLSS-5-Add-on hängt sich in die DLSS-Aufrufe des Spiels (beste Qualität, Bewegungsvektoren des Spiels). Lädt ReShade (Add-on-Version), RenoDX und NVIDIAs DLSS-5-Modell von GitHub.",
       "Jedes Spiel: DLSS5-Feeder ruft DLSS mit dem ReShade-Bild auf (Tiefe + LumeniteFX-Bewegungsvektoren), RenoDX wendet DLSS 5 an. 32-Bit-Spiele nutzen einen 64-Bit-Helfer, DirectX 8/9 läuft über dgVoodoo2.",
       "NVIDIAs öffentliches DLSS-5-Modell läuft nur auf RTX 50. Für RTX 20/30/40 zuerst eine für deine GPU gebaute nvngx_dlssnr.dll wählen (unten).",
       "Vulkan-Spiele brauchen den Installer von DLSS5-Feeder (systemweite ReShade-Ebene).", "Diese Grafik-API kann nicht an DLSS 5 übergeben werden.", "DLSS-5-Modell für diese GPU",
       "RTX 20/30/40: NVIDIAs Modell lehnt diese Karten ab; nötig ist eine für deine GPU angepasste nvngx_dlssnr.dll (aus der RenoDX-Community). MoonUp verteilt sie nicht.",
       "Modell {v} bereit.", "nvngx_dlssnr.dll wählen", "Trotzdem DLSS5-Feeder verwenden", "Wenn die DLSS-Anbindung des Spiels keine Wirkung zeigt, DLSS 5 stattdessen über ReShade füttern.",
       "DLSS 5: DLSS in den Spieleinstellungen einschalten (beliebige Qualität); Pos1 > Add-ons > RenoDX zeigt die DLSS-5-Regler.",
       "DLSS 5: Pos1 > Add-ons > RenoDX zeigt die DLSS-5-Regler; DLSS5_Feed und Lumenite_Kernel müssen aktiv bleiben. Protokoll: dlss5-feed.log.",
       "Suche DLSS-5-Komponenten", "Lade DLSS-5-Modell", "Lade RenoDX DLSS 5", "Lade DLSS-Laufzeit", "Lade DLSS5-Feeder", "Lade LumeniteFX", "Lade dgVoodoo2",
       "DLSS 5 und OptiScaler FSR 3.1 können nicht zusammen installiert werden.", "DLSS 5 braucht eine NVIDIA-GeForce-RTX-Grafikkarte.", "RTX 20/30/40: zuerst die für deine GPU gebaute nvngx_dlssnr.dll wählen.",
       "Vulkan-Spiele brauchen den Installer von DLSS5-Feeder.", "Eine DLSS-5-Komponente konnte nicht von GitHub geladen werden.", "Diese Datei ist keine 64-Bit-nvngx_dlssnr.dll.", "Dieses Spiel verbietet ReShade (ReShade-Kompatibilitätsliste)."],
"es": ["Upgraph", "Preparando", "Buscando ReShade", "Descargando ReShade", "Descomprimiendo ReShade", "Buscando OptiScaler", "Descargando OptiScaler", "Descomprimiendo OptiScaler", "Instalando", "Listo", "Quitando",
       "Ese archivo no es un instalador de ReShade.", "No se pudo descargar OptiScaler de GitHub.", "No se pudo descomprimir la descarga (el .7z de OptiScaler necesita Windows 11 23H2 o posterior).",
       "No se pudo escribir un archivo. ¿La carpeta del juego es de solo lectura o el disco está lleno?", "Este juego usa anti-cheat; confirma primero el aviso.", "No se encontró el ejecutable del juego.",
       "Los juegos Vulkan necesitan el instalador propio de ReShade.", "Esta API gráfica no es compatible.", "OptiScaler solo admite juegos de 64 bits.", "OptiScaler admite juegos DirectX 11/12 y Vulkan.",
       "Upgraph no está instalado en este juego.", "La carpeta del juego ya no existe.", "NVIDIA DLSS 5 (neural rendering)",
       "Este juego trae DLSS: el complemento RenoDX DLSS 5 se engancha a las llamadas DLSS del propio juego (mejor calidad, vectores de movimiento del juego). Descarga ReShade (versión con complementos), RenoDX y el modelo DLSS 5 de NVIDIA desde GitHub.",
       "Cualquier juego: DLSS5-Feeder hace las llamadas DLSS con la imagen de ReShade (profundidad + vectores de LumeniteFX) y RenoDX aplica DLSS 5. Los juegos de 32 bits usan un ayudante de 64 bits; DirectX 8/9 pasa por dgVoodoo2.",
       "El modelo público de DLSS 5 de NVIDIA solo funciona en RTX 50. Para RTX 20/30/40 elige antes un nvngx_dlssnr.dll hecho para tu GPU (abajo).",
       "Los juegos Vulkan necesitan el instalador propio de DLSS5-Feeder (capa ReShade de todo el sistema).", "Esta API gráfica no puede alimentar a DLSS 5.", "Modelo DLSS 5 para esta GPU",
       "RTX 20/30/40: el modelo de NVIDIA rechaza estas tarjetas; hace falta un nvngx_dlssnr.dll modificado para tu GPU (compartido por la comunidad RenoDX). MoonUp no lo distribuye.",
       "Modelo {v} listo.", "Elegir nvngx_dlssnr.dll", "Usar DLSS5-Feeder igualmente", "Si el enganche al DLSS del juego no tiene efecto, alimenta DLSS 5 desde ReShade.",
       "DLSS 5: activa DLSS (cualquier calidad) en los ajustes del juego; Inicio > Add-ons > RenoDX muestra los controles de DLSS 5.",
       "DLSS 5: Inicio > Add-ons > RenoDX muestra los controles de DLSS 5; DLSS5_Feed y Lumenite_Kernel deben seguir activos. Registro en dlss5-feed.log.",
       "Buscando componentes de DLSS 5", "Descargando el modelo DLSS 5", "Descargando RenoDX DLSS 5", "Descargando el runtime DLSS", "Descargando DLSS5-Feeder", "Descargando LumeniteFX", "Descargando dgVoodoo2",
       "DLSS 5 y OptiScaler FSR 3.1 no se pueden instalar juntos.", "DLSS 5 necesita una tarjeta NVIDIA GeForce RTX.", "RTX 20/30/40: elige primero el nvngx_dlssnr.dll hecho para tu GPU.",
       "Los juegos Vulkan necesitan el instalador propio de DLSS5-Feeder.", "No se pudo descargar un componente de DLSS 5 de GitHub.", "Ese archivo no es un nvngx_dlssnr.dll de 64 bits.", "Este juego prohíbe ReShade (lista de compatibilidad de ReShade)."],
"fr": ["Upgraph", "Préparation", "Recherche de ReShade", "Téléchargement de ReShade", "Décompression de ReShade", "Recherche d'OptiScaler", "Téléchargement d'OptiScaler", "Décompression d'OptiScaler", "Installation", "Terminé", "Retrait",
       "Ce fichier n'est pas un installateur ReShade.", "OptiScaler n'a pas pu être téléchargé depuis GitHub.", "Le téléchargement n'a pas pu être décompressé (le .7z d'OptiScaler nécessite Windows 11 23H2 ou plus).",
       "Un fichier n'a pas pu être écrit. Le dossier du jeu est-il en lecture seule ou le disque plein ?", "Ce jeu utilise un anti-triche ; confirme d'abord l'avertissement.", "Aucun exécutable de jeu trouvé.",
       "Les jeux Vulkan nécessitent l'installateur ReShade.", "Cette API graphique n'est pas prise en charge.", "OptiScaler ne prend en charge que les jeux 64 bits.", "OptiScaler prend en charge les jeux DirectX 11/12 et Vulkan.",
       "Upgraph n'est pas installé dans ce jeu.", "Le dossier du jeu n'existe plus.", "NVIDIA DLSS 5 (rendu neuronal)",
       "Ce jeu inclut DLSS : l'add-on RenoDX DLSS 5 s'accroche aux appels DLSS du jeu (meilleure qualité, vecteurs de mouvement du jeu). Télécharge ReShade (version add-on), RenoDX et le modèle DLSS 5 de NVIDIA depuis GitHub.",
       "Tout jeu : DLSS5-Feeder effectue les appels DLSS à partir de l'image ReShade (profondeur + vecteurs LumeniteFX), RenoDX applique DLSS 5. Les jeux 32 bits utilisent un assistant 64 bits, DirectX 8/9 passe par dgVoodoo2.",
       "Le modèle DLSS 5 public de NVIDIA ne fonctionne que sur RTX 50. Pour RTX 20/30/40, choisis d'abord un nvngx_dlssnr.dll conçu pour ta carte (ci-dessous).",
       "Les jeux Vulkan nécessitent l'installateur de DLSS5-Feeder (couche ReShade pour tout le système).", "Cette API graphique ne peut pas alimenter DLSS 5.", "Modèle DLSS 5 pour cette carte",
       "RTX 20/30/40 : le modèle NVIDIA refuse ces cartes ; il faut un nvngx_dlssnr.dll modifié pour ta carte (partagé par la communauté RenoDX). MoonUp ne le distribue pas.",
       "Modèle {v} prêt.", "Choisir nvngx_dlssnr.dll", "Utiliser DLSS5-Feeder quand même", "Si l'accroche au DLSS du jeu ne donne aucun effet, alimente DLSS 5 depuis ReShade.",
       "DLSS 5 : active DLSS (n'importe quelle qualité) dans les réglages du jeu ; Origine > Add-ons > RenoDX affiche les réglages DLSS 5.",
       "DLSS 5 : Origine > Add-ons > RenoDX affiche les réglages DLSS 5 ; DLSS5_Feed et Lumenite_Kernel doivent rester activés. Journal : dlss5-feed.log.",
       "Recherche des composants DLSS 5", "Téléchargement du modèle DLSS 5", "Téléchargement de RenoDX DLSS 5", "Téléchargement du runtime DLSS", "Téléchargement de DLSS5-Feeder", "Téléchargement de LumeniteFX", "Téléchargement de dgVoodoo2",
       "DLSS 5 et OptiScaler FSR 3.1 ne peuvent pas être installés ensemble.", "DLSS 5 nécessite une carte NVIDIA GeForce RTX.", "RTX 20/30/40 : choisis d'abord le nvngx_dlssnr.dll conçu pour ta carte.",
       "Les jeux Vulkan nécessitent l'installateur de DLSS5-Feeder.", "Un composant DLSS 5 n'a pas pu être téléchargé depuis GitHub.", "Ce fichier n'est pas un nvngx_dlssnr.dll 64 bits.", "Ce jeu interdit ReShade (liste de compatibilité ReShade)."],
"it": ["Upgraph", "Preparazione", "Ricerca di ReShade", "Download di ReShade", "Estrazione di ReShade", "Ricerca di OptiScaler", "Download di OptiScaler", "Estrazione di OptiScaler", "Installazione", "Fatto", "Rimozione",
       "Questo file non è un installer di ReShade.", "Impossibile scaricare OptiScaler da GitHub.", "Impossibile estrarre il download (il .7z di OptiScaler richiede Windows 11 23H2 o successivo).",
       "Impossibile scrivere un file. La cartella del gioco è di sola lettura o il disco è pieno?", "Questo gioco usa un anti-cheat; conferma prima l'avviso.", "Nessun eseguibile del gioco trovato.",
       "I giochi Vulkan richiedono l'installer di ReShade.", "Questa API grafica non è supportata.", "OptiScaler supporta solo giochi a 64 bit.", "OptiScaler supporta giochi DirectX 11/12 e Vulkan.",
       "Upgraph non è installato in questo gioco.", "La cartella del gioco non esiste più.", "NVIDIA DLSS 5 (neural rendering)",
       "Questo gioco ha DLSS: l'add-on RenoDX DLSS 5 si aggancia alle chiamate DLSS del gioco (qualità migliore, vettori di movimento del gioco). Scarica ReShade (versione add-on), RenoDX e il modello DLSS 5 di NVIDIA da GitHub.",
       "Qualsiasi gioco: DLSS5-Feeder esegue le chiamate DLSS dall'immagine di ReShade (profondità + vettori LumeniteFX), RenoDX applica DLSS 5. I giochi a 32 bit usano un helper a 64 bit, DirectX 8/9 passa per dgVoodoo2.",
       "Il modello DLSS 5 pubblico di NVIDIA funziona solo su RTX 50. Per RTX 20/30/40 scegli prima un nvngx_dlssnr.dll creato per la tua GPU (sotto).",
       "I giochi Vulkan richiedono l'installer di DLSS5-Feeder (layer ReShade di sistema).", "Questa API grafica non può alimentare DLSS 5.", "Modello DLSS 5 per questa GPU",
       "RTX 20/30/40: il modello NVIDIA rifiuta queste schede; serve un nvngx_dlssnr.dll modificato per la tua GPU (condiviso dalla community RenoDX). MoonUp non lo distribuisce.",
       "Modello {v} pronto.", "Scegli nvngx_dlssnr.dll", "Usa comunque DLSS5-Feeder", "Se l'aggancio al DLSS del gioco non ha effetto, alimenta DLSS 5 da ReShade.",
       "DLSS 5: attiva DLSS (qualsiasi qualità) nelle impostazioni del gioco; Home > Add-ons > RenoDX mostra i cursori di DLSS 5.",
       "DLSS 5: Home > Add-ons > RenoDX mostra i cursori di DLSS 5; DLSS5_Feed e Lumenite_Kernel devono restare attivi. Log: dlss5-feed.log.",
       "Ricerca dei componenti DLSS 5", "Download del modello DLSS 5", "Download di RenoDX DLSS 5", "Download del runtime DLSS", "Download di DLSS5-Feeder", "Download di LumeniteFX", "Download di dgVoodoo2",
       "DLSS 5 e OptiScaler FSR 3.1 non possono essere installati insieme.", "DLSS 5 richiede una scheda NVIDIA GeForce RTX.", "RTX 20/30/40: scegli prima il nvngx_dlssnr.dll creato per la tua GPU.",
       "I giochi Vulkan richiedono l'installer di DLSS5-Feeder.", "Impossibile scaricare un componente DLSS 5 da GitHub.", "Questo file non è un nvngx_dlssnr.dll a 64 bit.", "Questo gioco vieta ReShade (lista di compatibilità di ReShade)."],
"pt": ["Upgraph", "Preparando", "Procurando o ReShade", "Baixando o ReShade", "Descompactando o ReShade", "Procurando o OptiScaler", "Baixando o OptiScaler", "Descompactando o OptiScaler", "Instalando", "Pronto", "Removendo",
       "Esse arquivo não é um instalador do ReShade.", "Não foi possível baixar o OptiScaler do GitHub.", "Não foi possível descompactar o download (o .7z do OptiScaler exige Windows 11 23H2 ou mais novo).",
       "Não foi possível gravar um arquivo. A pasta do jogo é somente leitura ou o disco está cheio?", "Este jogo usa anti-cheat; confirme o aviso primeiro.", "Nenhum executável do jogo encontrado.",
       "Jogos Vulkan precisam do instalador do ReShade.", "Esta API gráfica não é suportada.", "O OptiScaler só suporta jogos de 64 bits.", "O OptiScaler suporta jogos DirectX 11/12 e Vulkan.",
       "O Upgraph não está instalado neste jogo.", "A pasta do jogo não existe mais.", "NVIDIA DLSS 5 (renderização neural)",
       "Este jogo tem DLSS: o complemento RenoDX DLSS 5 se conecta às chamadas DLSS do próprio jogo (melhor qualidade, vetores de movimento do jogo). Baixa o ReShade (versão com complementos), o RenoDX e o modelo DLSS 5 da NVIDIA do GitHub.",
       "Qualquer jogo: o DLSS5-Feeder faz as chamadas DLSS a partir da imagem do ReShade (profundidade + vetores do LumeniteFX) e o RenoDX aplica o DLSS 5. Jogos de 32 bits usam um ajudante de 64 bits; DirectX 8/9 passa pelo dgVoodoo2.",
       "O modelo público do DLSS 5 da NVIDIA só roda em RTX 50. Para RTX 20/30/40, escolha antes um nvngx_dlssnr.dll feito para a sua GPU (abaixo).",
       "Jogos Vulkan precisam do instalador do DLSS5-Feeder (camada do ReShade para o sistema todo).", "Esta API gráfica não pode alimentar o DLSS 5.", "Modelo DLSS 5 para esta GPU",
       "RTX 20/30/40: o modelo da NVIDIA recusa essas placas; é preciso um nvngx_dlssnr.dll modificado para a sua GPU (compartilhado pela comunidade RenoDX). A MoonUp não o distribui.",
       "Modelo {v} pronto.", "Escolher nvngx_dlssnr.dll", "Usar o DLSS5-Feeder mesmo assim", "Se a conexão ao DLSS do jogo não tiver efeito, alimente o DLSS 5 pelo ReShade.",
       "DLSS 5: ative o DLSS (qualquer qualidade) nas opções do jogo; Home > Add-ons > RenoDX mostra os controles do DLSS 5.",
       "DLSS 5: Home > Add-ons > RenoDX mostra os controles do DLSS 5; DLSS5_Feed e Lumenite_Kernel devem ficar ativos. Registro em dlss5-feed.log.",
       "Procurando componentes do DLSS 5", "Baixando o modelo DLSS 5", "Baixando o RenoDX DLSS 5", "Baixando o runtime DLSS", "Baixando o DLSS5-Feeder", "Baixando o LumeniteFX", "Baixando o dgVoodoo2",
       "DLSS 5 e OptiScaler FSR 3.1 não podem ser instalados juntos.", "O DLSS 5 precisa de uma placa NVIDIA GeForce RTX.", "RTX 20/30/40: escolha primeiro o nvngx_dlssnr.dll feito para a sua GPU.",
       "Jogos Vulkan precisam do instalador do DLSS5-Feeder.", "Não foi possível baixar um componente do DLSS 5 do GitHub.", "Esse arquivo não é um nvngx_dlssnr.dll de 64 bits.", "Este jogo proíbe o ReShade (lista de compatibilidade do ReShade)."],
"ru": ["Upgraph", "Подготовка", "Поиск ReShade", "Загрузка ReShade", "Распаковка ReShade", "Поиск OptiScaler", "Загрузка OptiScaler", "Распаковка OptiScaler", "Установка", "Готово", "Удаление",
       "Этот файл не является установщиком ReShade.", "Не удалось скачать OptiScaler с GitHub.", "Не удалось распаковать загрузку (.7z OptiScaler требует Windows 11 23H2 или новее).",
       "Не удалось записать файл. Папка игры только для чтения или диск заполнен?", "В игре есть античит; сначала подтвердите предупреждение.", "Исполняемый файл игры не найден.",
       "Для Vulkan-игр нужен установщик ReShade.", "Этот графический API не поддерживается.", "OptiScaler поддерживает только 64-битные игры.", "OptiScaler поддерживает игры DirectX 11/12 и Vulkan.",
       "Upgraph не установлен в этой игре.", "Папка игры больше не существует.", "NVIDIA DLSS 5 (нейронный рендеринг)",
       "В игре есть DLSS: дополнение RenoDX DLSS 5 перехватывает собственные вызовы DLSS игры (лучшее качество, векторы движения игры). Скачиваются ReShade (версия с дополнениями), RenoDX и модель DLSS 5 от NVIDIA с GitHub.",
       "Любая игра: DLSS5-Feeder сам вызывает DLSS по кадру ReShade (глубина + векторы LumeniteFX), RenoDX применяет DLSS 5. 32-битные игры используют 64-битного помощника, DirectX 8/9 идёт через dgVoodoo2.",
       "Публичная модель DLSS 5 от NVIDIA работает только на RTX 50. Для RTX 20/30/40 сначала укажите nvngx_dlssnr.dll, собранный для вашей видеокарты (ниже).",
       "Для Vulkan-игр нужен установщик DLSS5-Feeder (общесистемный слой ReShade).", "Этот графический API нельзя передать в DLSS 5.", "Модель DLSS 5 для этой видеокарты",
       "RTX 20/30/40: модель NVIDIA не принимает эти карты; нужен изменённый nvngx_dlssnr.dll для вашей видеокарты (распространяется сообществом RenoDX). MoonUp его не распространяет.",
       "Модель {v} готова.", "Указать nvngx_dlssnr.dll", "Всё равно использовать DLSS5-Feeder", "Если перехват DLSS игры не даёт эффекта, подайте DLSS 5 через ReShade.",
       "DLSS 5: включите DLSS (любое качество) в настройках игры; Home > Add-ons > RenoDX — ползунки DLSS 5.",
       "DLSS 5: Home > Add-ons > RenoDX — ползунки DLSS 5; DLSS5_Feed и Lumenite_Kernel должны оставаться включёнными. Журнал: dlss5-feed.log.",
       "Поиск компонентов DLSS 5", "Загрузка модели DLSS 5", "Загрузка RenoDX DLSS 5", "Загрузка среды DLSS", "Загрузка DLSS5-Feeder", "Загрузка LumeniteFX", "Загрузка dgVoodoo2",
       "DLSS 5 и OptiScaler FSR 3.1 нельзя установить вместе.", "Для DLSS 5 нужна видеокарта NVIDIA GeForce RTX.", "RTX 20/30/40: сначала укажите nvngx_dlssnr.dll для вашей видеокарты.",
       "Для Vulkan-игр нужен установщик DLSS5-Feeder.", "Не удалось скачать компонент DLSS 5 с GitHub.", "Этот файл не является 64-битным nvngx_dlssnr.dll.", "Эта игра запрещает ReShade (список совместимости ReShade)."],
"ja": ["Upgraph", "準備中", "ReShade を検索中", "ReShade をダウンロード中", "ReShade を展開中", "OptiScaler を検索中", "OptiScaler をダウンロード中", "OptiScaler を展開中", "インストール中", "完了", "削除中",
       "このファイルは ReShade のセットアップではありません。", "GitHub から OptiScaler をダウンロードできませんでした。", "ダウンロードを展開できませんでした（OptiScaler の .7z には Windows 11 23H2 以降が必要です）。",
       "ファイルを書き込めませんでした。ゲームフォルダーが読み取り専用か、ディスクがいっぱいです。", "このゲームはアンチチートを使用しています。先に警告を確認してください。", "ゲームの実行ファイルが見つかりません。",
       "Vulkan ゲームには ReShade 本体のインストーラーが必要です。", "このグラフィックス API は非対応です。", "OptiScaler は 64 ビットのゲームのみ対応です。", "OptiScaler は DirectX 11/12 と Vulkan のゲームに対応します。",
       "このゲームには Upgraph がインストールされていません。", "ゲームフォルダーが存在しません。", "NVIDIA DLSS 5（ニューラルレンダリング）",
       "このゲームは DLSS を搭載：RenoDX DLSS 5 アドオンがゲーム自身の DLSS 呼び出しにフックします（最高品質、ゲームのモーションベクトル）。ReShade（アドオン版）、RenoDX、NVIDIA の DLSS 5 モデルを GitHub からダウンロードします。",
       "どのゲームでも：DLSS5-Feeder が ReShade の画像（深度 + LumeniteFX のモーションベクトル）から DLSS を呼び出し、RenoDX が DLSS 5 を適用します。32 ビットのゲームは 64 ビットのヘルパー、DirectX 8/9 は dgVoodoo2 を経由します。",
       "NVIDIA の公開 DLSS 5 モデルは RTX 50 でのみ動作します。RTX 20/30/40 では、先に GPU 用に作られた nvngx_dlssnr.dll を選択してください（下）。",
       "Vulkan ゲームには DLSS5-Feeder 本体のインストーラーが必要です（システム全体の ReShade レイヤー）。", "このグラフィックス API は DLSS 5 に渡せません。", "この GPU 用の DLSS 5 モデル",
       "RTX 20/30/40：NVIDIA のモデルはこれらのカードを拒否します。GPU 用に改変された nvngx_dlssnr.dll（RenoDX コミュニティで共有）が必要です。MoonUp は配布しません。",
       "モデル {v} の準備完了。", "nvngx_dlssnr.dll を選択", "それでも DLSS5-Feeder を使う", "ゲームの DLSS へのフックで効果がない場合、DLSS 5 を ReShade から供給します。",
       "DLSS 5：ゲーム設定で DLSS をオン（品質は任意）。Home > Add-ons > RenoDX に DLSS 5 のスライダーがあります。",
       "DLSS 5：Home > Add-ons > RenoDX に DLSS 5 のスライダーがあります。DLSS5_Feed と Lumenite_Kernel は有効のままにしてください。ログ：dlss5-feed.log。",
       "DLSS 5 コンポーネントを検索中", "DLSS 5 モデルをダウンロード中", "RenoDX DLSS 5 をダウンロード中", "DLSS ランタイムをダウンロード中", "DLSS5-Feeder をダウンロード中", "LumeniteFX をダウンロード中", "dgVoodoo2 をダウンロード中",
       "DLSS 5 と OptiScaler FSR 3.1 は同時にインストールできません。", "DLSS 5 には NVIDIA GeForce RTX が必要です。", "RTX 20/30/40：先に GPU 用の nvngx_dlssnr.dll を選択してください。",
       "Vulkan ゲームには DLSS5-Feeder 本体のインストーラーが必要です。", "GitHub から DLSS 5 コンポーネントをダウンロードできませんでした。", "このファイルは 64 ビットの nvngx_dlssnr.dll ではありません。", "このゲームは ReShade を禁止しています（ReShade 互換リスト）。"],
"zh": ["Upgraph", "准备中", "正在查找 ReShade", "正在下载 ReShade", "正在解压 ReShade", "正在查找 OptiScaler", "正在下载 OptiScaler", "正在解压 OptiScaler", "正在安装", "完成", "正在移除",
       "该文件不是 ReShade 安装程序。", "无法从 GitHub 下载 OptiScaler。", "无法解压下载内容（OptiScaler 的 .7z 需要 Windows 11 23H2 或更新版本）。",
       "无法写入文件。游戏文件夹是否只读或磁盘已满？", "此游戏使用反作弊，请先确认警告。", "未找到游戏可执行文件。",
       "Vulkan 游戏需要 ReShade 自带安装程序。", "不支持该图形 API。", "OptiScaler 仅支持 64 位游戏。", "OptiScaler 支持 DirectX 11/12 和 Vulkan 游戏。",
       "此游戏未安装 Upgraph。", "游戏文件夹已不存在。", "NVIDIA DLSS 5（神经渲染）",
       "此游戏自带 DLSS：RenoDX DLSS 5 插件挂接游戏自身的 DLSS 调用（最佳画质，使用游戏的运动矢量）。会从 GitHub 下载 ReShade（插件版）、RenoDX 和 NVIDIA 的 DLSS 5 模型。",
       "任何游戏：DLSS5-Feeder 根据 ReShade 画面（深度 + LumeniteFX 运动矢量）自行调用 DLSS，由 RenoDX 应用 DLSS 5。32 位游戏使用 64 位辅助程序，DirectX 8/9 通过 dgVoodoo2。",
       "NVIDIA 公开的 DLSS 5 模型仅在 RTX 50 上运行。RTX 20/30/40 请先选择为你的显卡制作的 nvngx_dlssnr.dll（见下方）。",
       "Vulkan 游戏需要 DLSS5-Feeder 自带安装程序（系统级 ReShade 层）。", "该图形 API 无法提供给 DLSS 5。", "此显卡的 DLSS 5 模型",
       "RTX 20/30/40：NVIDIA 的模型会拒绝这些显卡；需要为你的显卡修改过的 nvngx_dlssnr.dll（由 RenoDX 社区分享）。MoonUp 不分发该文件。",
       "模型 {v} 已就绪。", "选择 nvngx_dlssnr.dll", "仍使用 DLSS5-Feeder", "如果挂接游戏 DLSS 没有效果，改为通过 ReShade 提供 DLSS 5。",
       "DLSS 5：在游戏设置中开启 DLSS（任意画质）；Home > Add-ons > RenoDX 中有 DLSS 5 滑块。",
       "DLSS 5：Home > Add-ons > RenoDX 中有 DLSS 5 滑块；DLSS5_Feed 与 Lumenite_Kernel 必须保持开启。日志：dlss5-feed.log。",
       "正在查找 DLSS 5 组件", "正在下载 DLSS 5 模型", "正在下载 RenoDX DLSS 5", "正在下载 DLSS 运行时", "正在下载 DLSS5-Feeder", "正在下载 LumeniteFX", "正在下载 dgVoodoo2",
       "DLSS 5 与 OptiScaler FSR 3.1 不能同时安装。", "DLSS 5 需要 NVIDIA GeForce RTX 显卡。", "RTX 20/30/40：请先选择为你的显卡制作的 nvngx_dlssnr.dll。",
       "Vulkan 游戏需要 DLSS5-Feeder 自带安装程序。", "无法从 GitHub 下载 DLSS 5 组件。", "该文件不是 64 位 nvngx_dlssnr.dll。", "此游戏禁止 ReShade（ReShade 兼容列表）。"],
}


def main():
    for code, vals in T.items():
        assert len(vals) == len(KEYS), (code, len(vals), len(KEYS))
        path = os.path.join(ROOT, "ui/js/i18n/%s.js" % code)
        text = open(path, encoding="utf-8").read()
        add = []
        for k, v in zip(KEYS, vals):
            if re.search(r'\n\s*"%s":' % re.escape(k), text):
                text = re.sub(r'(\n\s*"%s":\s*)"(?:[^"\\]|\\.)*"' % re.escape(k), lambda m: m.group(1) + json.dumps(v, ensure_ascii=False), text)
            else:
                add.append("  %s: %s" % (json.dumps(k), json.dumps(v, ensure_ascii=False)))
        if add:
            end = text.rstrip().rindex("}")
            body = text[:end].rstrip()
            if not body.endswith(","):
                body += ","
            text = body + "\n" + ",\n".join(add) + "\n};\n"
        open(path, "w", encoding="utf-8").write(text)
        print(code, len(add))


if __name__ == "__main__":
    main()
