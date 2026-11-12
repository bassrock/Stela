# Handoff: supporto AMD Radeon RX 9070 (RDNA4) in Stela

Documento di passaggio da una sessione cloud di analisi a una sessione locale.
Nessun codice è stato scritto: finora c'è solo l'analisi di fattibilità. Tutto
ciò che segue è marcato come **verificato** (letto nei sorgenti) o **da
verificare** (dedotto o ricordato, non controllato).

- Data dell'analisi: 2026-11-12
- Branch di lavoro: `claude/amd-radeon-rx9070-rdna4-y12vd2`
- Repository: fork `LBNM-ONE/Stela` di `tylertms/Stela`. Alla data dell'analisi
  il fork coincide con l'upstream (`f39ffb3`).

## Obiettivo

Far funzionare il device Metal di Stela su una AMD Radeon RX 9070 (Navi 48,
RDNA4, GFX12). Oggi Stela supporta solo NVIDIA (GTX 16 / RTX 20 e successive).

## Hardware dell'utente

- CPU: AMD Ryzen 9 7900X, con grafica integrata (Raphael, RDNA2).
- GPU: AMD Radeon RX 9070.
- Il sistema operativo del PC locale non è noto. I controlli hardware elencati
  sotto richiedono Linux (anche una live USB).

## Verdetto

Fattibile, e l'architettura è adatta, ma è un port grosso: mesi, non giorni.
Il costo dominante è portare il driver kernel `amdgpu` nell'ambiente Linux
ospitato di Stela. Ogni verifica reale richiede di avviare Stela sul PC.

## Come Stela gestisce NVIDIA oggi (verificato)

Tre strati, tutti dentro `stela.efi`:

1. **Driver kernel**: `nouveau` di Linux, compilato senza modifiche in un
   ambiente Linux "ospitato" (`crates/stela-linux`). La build legge il Kbuild
   del driver (`crates/stela-driver-gpu/build/kbuild.rs`), compila gli oggetti
   (`build/nouveau.rs`) e confronta i simboli kernel importati con una lista
   esplicita di servizi mancanti (`OPEN` in `build/nouveau.rs`): la build
   fallisce se gli import cambiano.
2. **Driver Vulkan**: NVK di Mesa, compilato per `stela.efi` contro una libc
   propria (`xtask/src/gpu/nvk.rs`, `crates/stela-driver-gpu/src/compat/`).
   libdrm è sostituito da `compat/mesa/drm.c`, che è generico (ioctl DRM).
3. **Device Metal**: `crates/stela-driver-gpu/src/metal/`. Traduce Metal in
   Vulkan (binding `ash`) e ricava le capacità dalle feature del driver Vulkan,
   registrando i "gap" (`metal/capabilities.rs`). Gli shader passano come NIR
   tramite `VkPipelineShaderStageNirCreateInfoMESA` (`metal/native.rs`,
   `metal/native/objects.rs`).

Altri punti verificati:

- Il display usa ioctl KMS generici (dumb buffer + page flip):
  `crates/stela-boot/src/engine/gpu/display.rs`, `metal/kms`. Il commento in
  testa al file dice: "A GPU driving no display leaves the firmware's scanout
  alone".
- Il firmware NVIDIA (GSP) viene scaricato da linux-firmware a un commit
  pinnato: `crates/stela-loader/src/host_firmware/recipes.rs`.
- Il boot reclama le GPU in `crates/stela-boot/src/engine/gpu.rs`
  (`claim` riga 940, `claim_all` riga 1280, `open` riga 1574). È cablato su
  NVIDIA: vendor `0x10de`, `DMA_LIMIT` a 47 bit, attesa del firmware boot,
  selezione firmware tramite `nouveau_chip`.
- `crates/stela-driver-gpu/src/cold.rs` richiede un Function Level Reset
  (FLR) e rifiuta i vendor diversi da NVIDIA.
- Il remapping DMA per IOMMU AMD esiste già
  (`crates/stela-driver-base/src/bus/dma/amd`).

## Versioni pinnate (verificato)

| Componente | Pin | Versione |
|---|---|---|
| Linux (`third_party/linux`) | `cc9de5d4b5ab6459f5774946aa3195ad51236fa8` | 7.2.12 |
| Mesa (`third_party/mesa`) | `ddaf85e353442d9d7f192fdcf1823e652e3f2ee3` | 26.2.5 |
| Rust | `rust-toolchain.toml` | 1.99.1 |

## Cosa esiste già per AMD (verificato)

- `crates/stela-driver-gpu/src/lib.rs:96`: il vendor AMD è riconosciuto ma
  rifiutato con "AMDGPU is not ported to Stela".
- Lane del compilatore shader AMD: AIR → NIR → ACO (e LLVM), in
  `crates/stela-driver-gpu/src/amd/hosted/` con test in `src/amd/tests/`.
  È solo compilazione su CPU: nessun accesso alla GPU.
- `crates/stela-driver-gpu/src/compiler/tests/targets/amd.h` include già
  `CHIP_GFX1201`, che in Mesa è "Radeon 9070". Solo `CHIP_GFX1210` è escluso
  da ACO.
- `third_party/libdrm.sparse` porta solo gli header di libdrm e `amdgpu`,
  nessun sorgente.
- `third_party/mesa.sparse` non esclude `src/amd/vulkan` (RADV).
- `crates/stela-linux/src/compat/generated/autoconf.h` è il Kconfig di Stela,
  scritto a mano. Non contiene alcuna opzione `CONFIG_DRM_AMD*`.

## Fatti su amdgpu nel Linux pinnato (verificato)

Dimensioni, righe di `.c` e `.h` in `drivers/gpu/drm/amd/`:

| Directory | Righe |
|---|---|
| `amdgpu` (core) | 386.942 |
| `pm` (power management) | 185.444 |
| `display` (DC, DMUB, amdgpu_dm) | 592.886 |
| `amdkfd` | 48.255 |
| `ras` | 14.381 |
| `include` (senza `asic_reg`) | 138.431 |

- GC 12.0.0 e 12.0.1 usano `soc24_common_ip_block` (`amdgpu_discovery.c`).
- MES è abilitato per questi IP (`enable_mes`, `enable_uni_mes`).
- `gmc_v12_0.c` imposta la maschera DMA a 44 bit, oppure 52 in un secondo
  ramo. Quale ramo valga per Navi 48 è da leggere.
- `amdgpu_irq.c` alloca un solo vettore di interrupt
  (`pci_alloc_irq_vectors(pdev, 1, 1, flags)`).
- Firmware che il kernel dichiara per questa generazione:
  `gc_12_0_1_{pfp,me,mec,rlc,imu,toc,mes,mes1,uni_mes}.bin`,
  `gc_12_0_1_{imu,rlc}_kicker.bin`, `psp_14_0_3_{sos,ta}.bin` e varianti
  `_kicker`, `smu_14_0_3.bin` e `_kicker`, `sdma_7_0_1.bin`.
- Il `Makefile` di amdgpu usa sintassi che `build/kbuild.rs` oggi rifiuta:
  continuazioni di riga con `\`, variabili semplici (`FULL_AMD_PATH=...`),
  `ifneq ($(CONFIG_X),)`, `subdir-ccflags-y`, `$(addprefix ...)`, include di
  altri Makefile tramite variabile. `kbuild.rs` rifiuta di proposito ciò che è
  fuori dal suo sottoinsieme, anche nei rami inattivi.
- Il display core usa la FPU in kernel (35 file con `DC_FP_START` o
  `kernel_fpu_begin`).

## Fatti su RADV nel Mesa pinnato (verificato)

- Dimensioni: RADV 115.417 righe, ACO 75.485, `src/amd/common` 82.863.
- `radv_has_pops()` vale per GFX9 e successive con ACO, quindi RADV espone
  `VK_EXT_fragment_shader_interlock` anche su GFX12. Stela usa questa feature
  per i raster order group e le letture del framebuffer.
- `dynamicRenderingLocalRead` è esposto.
- RADV accetta NIR solo tramite `vk_shader_module.nir`
  (`radv_pipeline.c`, `internal_nir`). Non legge
  `VkPipelineShaderStageNirCreateInfoMESA`: serve una piccola patch Mesa o un
  adattamento lato Stela.
- Esiste solo il winsys `amdgpu` (`src/amd/vulkan/winsys/amdgpu`).
- Funzioni di `libdrm_amdgpu` che Mesa chiama, da fornire in `compat/mesa`:
  `amdgpu_bo_alloc`, `amdgpu_bo_cpu_map`, `amdgpu_bo_cpu_unmap`,
  `amdgpu_bo_export`, `amdgpu_bo_free`, `amdgpu_bo_import`,
  `amdgpu_create_bo_from_user_mem`, `amdgpu_cs_calculate_timeout`,
  `amdgpu_device_deinitialize`, `amdgpu_device_get_fd`,
  `amdgpu_device_initialize`, `amdgpu_get_marketing_name`,
  `amdgpu_ioctl_wait_cs`, `amdgpu_query_sw_info`, `amdgpu_va_range_alloc`,
  `amdgpu_va_range_free`, `amdgpu_va_range_query`.
- 20 delle 26 patch in `third_party/patches/mesa/` toccano `src/nouveau`:
  interlock, scratch degli shader, bind VA asincroni, gestione dei rifiuti di
  submit. Su RADV vanno rivalutate una per una.
- Hook specifici di NVK usati dal device Metal: `compat/mesa/submit_thread.c`
  (`stela_nvk_submit_thread`, `stela_nvk_submit_failure` e simili) e
  `compat/mesa/nir_options.c`.

## Da verificare

- Che Navi 48 usi proprio PSP 14.0.3, SMU 14.0.3 e SDMA 7.0.1. La
  corrispondenza è dedotta dai nomi. `dmesg` sul PC lo conferma.
- L'ID PCI della RX 9070. Ricordo `1002:7550`, non controllato.
- Se la RX 9070 supporta il Function Level Reset. `cold.rs` lo richiede. Se
  manca, serve un'altra via: reset del bus secondario sul bridge a monte,
  oppure il reset interno di amdgpu.
- Se, con la 9070 senza display e il monitor sulla iGPU, il desktop di macOS
  arriva davvero sullo schermo del firmware. Il commento in `gpu/display.rs`
  lo suggerisce, ma il percorso dal DCP modellato allo scanout del firmware
  non è stato tracciato.
- Quanti servizi kernel mancano ad amdgpu nell'ambiente ospitato. È
  l'incognita principale e la misura il primo passo qui sotto.
- Se l'upstream sta già lavorando su AMD. Il README annuncia altro supporto
  hardware e `autoconf.h` contiene già default per i915/xe. Conviene chiedere
  sul loro Discord prima di investire mesi.

## Cosa manca, per strato

1. **Kernel**: `amdgpu` nell'ambiente ospitato.
   - Estendere `build/kbuild.rs` oppure dichiarare una lista di oggetti
     verificata contro il Makefile.
   - Allargare `third_party/linux.sparse` con `/drivers/gpu/drm/amd/` e ciò
     che emerge dal censimento.
   - Aggiungere le opzioni a `autoconf.h`: `CONFIG_DRM_AMDGPU`, lasciando
     spenti DC, KFD, ACP, ISP, CIK, SI e USERPTR.
   - Fornire i servizi kernel mancanti in `crates/stela-linux/src/compat/kernel`
     e `crates/stela-driver-gpu/src/compat/linux/kernel`.
2. **Firmware**: ricette linux-firmware per i blob amdgpu, sul modello GSP in
   `recipes.rs`, e selezione per IP invece che per chipset.
3. **Boot**: generalizzare `claim`, `claim_all` e `cold.rs` oltre NVIDIA.
4. **Vulkan**: una lane RADV accanto a `xtask/src/gpu/nvk.rs`
   (`-Dvulkan-drivers=amd`), con ACO in C++ contro la libc++ già costruita per
   LLVM, il compat `libdrm_amdgpu` e l'aggancio NIR.
5. **Metal**: un modulo `metal/amd.rs` accanto a `metal/nvidia.rs`
   (ammissione del device, `Platform` con allineamenti e pagina sparse) e
   sostituti per gli hook NVK.

## Piano a tappe proposto

0. **Censimento amdgpu** (nessun hardware). Vedi sotto.
1. **RADV nell'immagine**: build e link in `stela.efi`.
2. **amdgpu senza display**: bind della 9070, firmware caricato, ioctl
   `AMDGPU_INFO` che rispondono. Monitor sulla iGPU, console del firmware
   viva per i log.
3. **Primo triangolo**: RADV su amdgpu, device Metal ammesso, suite Metal
   (`cargo xtask gpu metal-suite`).
4. **Display**: DC e DMUB per pilotare le uscite della 9070. Metà del codice
   kernel, rimandabile finché il monitor sta sulla iGPU.

## Primo passo: censimento amdgpu

Scopo: misurare il lavoro kernel prima di impegnarsi.

1. Prerequisiti: Rust (da `rust-toolchain.toml`), LLVM con Clang 21 o più
   recente, Python 3.10 o più recente.
2. `cargo xtask gpu fetch`. Scarica Linux, Mesa, LLVM, libdrm e xnu ai commit
   pinnati, in checkout sparse, e applica le patch di Stela. È lungo.
3. Aggiungere `/drivers/gpu/drm/amd/` a `third_party/linux.sparse` e rifare
   il fetch.
4. Creare `crates/stela-driver-gpu/build/amdgpu.rs` sul modello di
   `build/nouveau.rs`, dietro una feature `amdgpu-port`.
5. Compilare gli oggetti di amdgpu con DC spento e leggere l'elenco dei
   simboli kernel che nulla fornisce (`check_imports`).
6. Raggruppare l'elenco per sottosistema e stimare il lavoro.

## Controlli sul PC (Linux)

```sh
sudo lspci -nnvv -d 1002:
sudo dmesg | grep -i amdgpu
```

Nel primo cercare: gli ID dei due dispositivi, `FLReset+` o `FLReset-` in
`DevCap`, le dimensioni dei BAR, MSI e MSI-X. Nel secondo: le versioni degli
IP e i firmware caricati. Annotare anche dove è collegato il monitor e
l'impostazione del BIOS per la grafica integrata.

## Regole del repository osservate

- Messaggi di commit in stile `tipo(ambito): frase descrittiva lunga`, per
  esempio `perf(gpu): ...`, `feat(boot): ...`.
- I sorgenti in `third_party/` non si modificano a mano: si lavora nel
  submodule e si esporta la serie con `cargo xtask gpu export` in
  `third_party/patches/<nome>/`.
- Il C di Stela e le unità Mesa che Stela compila da sé usano `-Werror`.
- `cargo xtask check` esegue formattazione, Clippy e test. La CI esegue
  formattazione, Clippy e i test di `stela-cli`.
- Licenza: GPL-2.0-or-later per il codice di Stela; i port da Linux tengono
  GPL-2.0-only, Mesa MIT.

## Come sono stati ispezionati i sorgenti upstream

I submodule non erano scaricati. I commit pinnati sono stati letti con fetch
parziali fuori dal repository, per esempio:

```sh
git init linux && cd linux
git remote add origin https://github.com/gregkh/linux.git
git sparse-checkout init --no-cone
git sparse-checkout set '/Makefile' '/drivers/gpu/drm/amd/' \
  '!/drivers/gpu/drm/amd/include/asic_reg/'
git fetch --depth 1 --filter=blob:none origin cc9de5d4b5ab6459f5774946aa3195ad51236fa8
git checkout FETCH_HEAD
```

Lo stesso per Mesa da `https://gitlab.freedesktop.org/mesa/mesa.git` al
commit `ddaf85e353442d9d7f192fdcf1823e652e3f2ee3`, con `/src/amd/`.

## Stato

- Nessuna build è stata eseguita e nessun test è stato lanciato.
- Nessun file del progetto è stato modificato, a parte questo documento.
- Decisione in sospeso: partire dal censimento amdgpu (consigliato) oppure
  sentire prima l'upstream.
