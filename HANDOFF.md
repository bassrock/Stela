# Handoff: supporto AMD Radeon RX 9070 (RDNA4) in Stela

Documento di passaggio da una sessione cloud di analisi a una sessione locale.
All'analisi di fattibilità sono seguiti, sul PC, il censimento e i passi 1–4
del port (vedi «Censimento amdgpu: risultati» e «Stato»). Tutto ciò che
segue è marcato come **verificato** (letto nei sorgenti) o **da verificare**
(dedotto o ricordato, non controllato).

- Data dell'analisi: 2026-11-12
- Branch di lavoro: `claude/amd-radeon-rx9070-rdna4-y12vd2`
- Repository: fork `LBNM-ONE/Stela` di `tylertms/Stela`. Alla data dell'analisi
  il fork coincide con l'upstream (`f39ffb3`).
- Aggiornato il 2026-11-12 dalla sessione locale sul PC con l'hardware:
  sezioni «Verificato sul PC», «Ambiente di build sul PC» e «Stato».
- Aggiornato il 2026-11-13 con il passo 4 e «Prova sul PC (tappa 2)».

## Obiettivo

Far funzionare il device Metal di Stela su una AMD Radeon RX 9070 (Navi 48,
RDNA4, GFX12). Oggi Stela supporta solo NVIDIA (GTX 16 / RTX 20 e successive).

## Hardware dell'utente

- CPU: AMD Ryzen 9 7900X, con grafica integrata (Raphael, RDNA2).
- GPU: AMD Radeon RX 9070.
- Sistema operativo del PC: Ubuntu 26.04.1 LTS, kernel 7.0.0-43-generic
  (dettagli in «Verificato sul PC»).

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

## Verificato sul PC (2026-11-12)

Letto sul PC da `lspci -nn`, sysfs e `journalctl -k`, senza `sudo`.

- Sistema: Ubuntu 26.04.1 LTS, kernel 7.0.0-43-generic, avvio UEFI, Secure
  Boot disattivato. Scheda madre Gigabyte B850 AORUS ELITE WIFI7 ICE, BIOS
  F12c del 2026-07-21. ESP su `/boot/efi` (`nvme0n1p2`) con `BOOT`,
  `Microsoft` e `ubuntu`. 29 GB di RAM, 125 GB liberi su disco.
- RX 9070: `0000:03:00.0`, ID PCI `1002:7550` rev `c3`, subsystem
  `1da2:4499`, VBIOS `113-4E490TX-UP5`, 16 GB GDDR6 a 256 bit (VRAM 16304M),
  56 CU attive (4 SE, 2 SH per SE, 8 CU per SH). Funzione audio `03:00.1`
  (`1002:ab40`). La scheda sta dietro il proprio switch PCIe: upstream port
  `01:00.0` (`1002:1478`), downstream port `02:00.0` (`1002:1479`). È il
  `boot_vga`.
- iGPU Raphael: `0000:12:00.0`, ID `1002:164e` rev `c2`, GC 10.3.6, DCN
  3.1.5, 512 MB di VRAM, pilotata da amdgpu: la grafica integrata è attiva
  nel BIOS. Nessuna uscita collegata.
- Monitor: due, entrambi sulla 9070 (`card1-DP-2` e `card1-HDMI-A-2`
  connessi e abilitati). Le uscite della iGPU (`card0-DP-3/4/5`,
  `card0-HDMI-A-3`) sono scollegate. Per la tappa 2 del piano i monitor
  vanno spostati sulle uscite della scheda madre.
- Reset: `/sys/bus/pci/devices/0000:03:00.0/reset_method` elenca solo `bus`,
  quindi il kernel non trova FLR (né reset PM o ACPI) sulla 9070; lo stesso
  vale per la iGPU. Il reset del bus secondario passa dal downstream port
  `02:00.0` e coinvolge anche `03:00.1`. amdgpu usa BACO per il runtime pm.
  Il bit `FLReset` di `DevCap` resta da leggere con
  `sudo lspci -nnvv -s 03:00.0`.
- Versioni IP della 9070 (da `ip_discovery`, 46 IP): GC 12.0.1, MP0 (PSP)
  14.0.3, MP1 (SMU) 14.0.3, SDMA0 e SDMA1 7.0.1, LSDMA 7.0.1, DMU 4.1.0 (DCN
  4.0.1), VCN 5.0.0, NBIF 6.3.1, MMHUB 4.1.0, ATHUB 4.1.0, HDP 7.0.0, OSSSYS
  7.0.0, UMC 8.14.0 (8 istanze), SMUIO 14.0.2, THM 14.0.2, DF 4.15.1,
  SYSTEMHUB 2.3.0, PWR 9.0.0. PSP 14.0.3, SMU 14.0.3 e SDMA 7.0.1 sono
  confermati.
- Blocchi IP che amdgpu 3.64.0 (kernel 7.0) istanzia per la 9070:
  soc24_common, gmc_v12_0, ih_v7_0, psp_v14_0, smu_v14_0, dm, gfx_v12_0,
  sdma_v7_0, vcn_v5_0_0, jpeg_v5_0_0, mes_v12_0. MES: `gfx_hqd_mask 0xfe`,
  `compute_hqd_mask 0x0c`, `sdma_hqd_mask 0xfc`. Ring: gfx_0.0.0,
  comp_1.0.0, comp_1.1.0, comp_1.0.1, comp_1.1.1, sdma0, sdma1,
  vcn_unified_0, jpeg_dec.
- Firmware caricati sulla 9070: sos 0x003a1014, ta_ras 0x1b3a0001, imu
  0x0c302b00, pfp 0xbea, me 0xb9a, mec 0xcda, mes e mes_kiq 0x89, rlc
  0x00be7da0, sdma 0x00798e96, smc 0x00684c00 (104.76.0), dmcub 0x0a000800,
  vcn 0x0910c00f. I file sono in `/lib/firmware/amdgpu/`, compressi `.zst`:
  `gc_12_0_1_{pfp,me,mec,mes,mes1,rlc,imu}.bin` e varianti `_kicker`,
  `psp_14_0_3_{sos,ta}.bin` e `_kicker`, `smu_14_0_3.bin` e `_kicker`,
  `sdma_7_0_1.bin`, `dcn_4_0_1_dmcub.bin`; `gc_12_0_1_toc.bin` e
  `gc_12_0_1_uni_mes.bin` sono link a `gc_12_0_0_*`. TMZ non supportato; le
  TA opzionali RAP e SECUREDISPLAY mancano.
- Memoria e BAR della 9070: il firmware lascia BAR0 a 256 MB
  (`0xfce0000000`); è amdgpu che lo allarga a 16 GB (Resizable BAR, BAR =
  VRAM) e lo sposta a `0xc00000000`. BAR2 2 MB (doorbell) a `0x1000000000`,
  registri MMIO 512 KB a `0xdfc00000`, I/O a `0xf000`. GART 512M, vm size
  262144 GB (4 livelli, blocchi e frammenti da 9 bit).
- Interrupt: un solo vettore MSI per la 9070 (`msi_irqs` = 1).
- IOMMU: AMD IOMMU attivo (`ivhd0`). Gruppi: `03:00.0` → 14 da sola,
  `03:00.1` → 15, `02:00.0` → 13, `01:00.0` → 12, `12:00.0` → 29.

## Da verificare

- Il bit `FLReset` in `DevCap` della 9070 (`sudo lspci -nnvv -s 03:00.0`).
  sysfs dice già che il kernel non trova FLR; se `DevCap` lo conferma,
  `cold.rs` ha bisogno di un'altra via: reset del bus secondario sul
  downstream port `02:00.0`, oppure il reset interno di amdgpu. Letto il
  2026-11-13: `soc24_asic_reset_method` sceglie MODE1 (reset via SMU) per
  MP1 14.0.2 e 14.0.3, quindi per la 9070, e non sceglie mai il reset PCI.
  Deciso nel passo 4: per AMD `cold.rs` non resetta (`Reset::Driver`) e
  lascia il reset ad amdgpu. Il bit dice solo se un FLR sarebbe possibile.
- Se il BIOS («Re-Size BAR Support», con «Above 4G Decoding») fa assegnare
  al firmware un BAR0 da 16 GB. Oggi il firmware lo lascia a 256 MB e su
  Linux è amdgpu ad allargarlo; in Stela il resize è rifiutato (vedi il
  passo 2), quindi amdgpu userebbe 256 MB di VRAM visibile alla CPU.
- Il primo probe di amdgpu sulla 9070 in Stela: niente è stato eseguito
  sull'hardware (vedi «Prova sul PC (tappa 2)»). Gli 11 file firmware sono
  quelli che amdgpu chiede di default, letti nei sorgenti. Se il log dice
  che ne manca uno, va aggiunto a `AMDGPU` in `recipes.rs`.
- Come amdgpu trova il VBIOS. Stela non gli dà né ACPI (VFCT) né il BAR
  della ROM, quindi resta la lettura della ROM via MMIO
  (`amdgpu_read_bios_from_rom`). Con la iGPU come prima uscita, il firmware
  potrebbe non inizializzare la 9070: in quel caso amdgpu la inizializza
  dal VBIOS (`amdgpu_device_need_post`).
- Se, con la 9070 senza display e il monitor sulla iGPU, il desktop di macOS
  arriva davvero sullo schermo del firmware. Il commento in `gpu/display.rs`
  lo suggerisce, ma il percorso dal DCP modellato allo scanout del firmware
  non è stato tracciato. Oggi i monitor stanno sulla 9070.
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
     e `crates/stela-driver-gpu/src/compat/linux/kernel`. Misurati il
     2026-11-12: 82 propri di amdgpu, 55 dopo il passo 1, 39 dopo il passo
     2; dopo il passo 3 ogni servizio che la 9070 raggiunge è fornito e gli
     altri sono trap, tranne `sscanf`, vedi «Censimento amdgpu: risultati».
   - Passo 4: amdgpu è linkato in `stela.efi`, `sscanf` è fornito e i 29
     servizi aperti sono tutti trap.
2. **Firmware**: fatto nel passo 4 per Navi 48: 11 file e la licenza in
   `recipes.rs` (`AMDGPU`), scelti per ID PCI, nel bundle della chiavetta.
3. **Boot**: fatto nel passo 4: `claim` e `claim_all` per vendor, e
   `cold.rs` senza reset per AMD.
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
   recente, Python 3.10 o più recente. Fatto il 2026-11-12: vedi «Ambiente
   di build sul PC» per il PATH.
2. `cargo xtask gpu fetch`. Scarica Linux, Mesa, LLVM, libdrm e xnu ai commit
   pinnati, in checkout sparse, e applica le patch di Stela. Fatto il
   2026-11-12: due minuti e mezzo su questo PC.
3. Aggiungere `/drivers/gpu/drm/amd/` a `third_party/linux.sparse` e rifare
   il fetch. Fatto il 2026-11-12, prima del primo fetch.
4. Creare `crates/stela-driver-gpu/build/amdgpu.rs` sul modello di
   `build/nouveau.rs`, dietro una feature `amdgpu-port`. Fatto il 2026-11-12.
5. Compilare gli oggetti di amdgpu con DC spento e leggere l'elenco dei
   simboli kernel che nulla fornisce (`check_imports`). Fatto il 2026-11-12:
   91 servizi, pinnati nella lista `OPEN` di `amdgpu.rs`.
6. Raggruppare l'elenco per sottosistema e stimare il lavoro. Fatto il
   2026-11-12: sezione seguente.

## Censimento amdgpu: risultati (2026-11-12)

Eseguito sul PC con `cargo build -p stela-driver-gpu --features amdgpu-port`
(toolchain nel PATH come in «Ambiente di build sul PC»; sei minuti la prima
volta, con le dipendenze e l'ambiente di `stela-linux`, un minuto e
tre quarti quando ricompila solo classe DRM, nouveau e amdgpu). La
feature compila i 376 oggetti di `amdgpu-y` invariati nell'ambiente
ospitato, sopra l'archivio DRM del port nouveau, e confronta gli import che
nulla fornisce con la lista `OPEN` di `crates/stela-driver-gpu/build/amdgpu.rs`,
come `check_imports` fa per nouveau; il dettaglio, con gli oggetti che
importano ogni servizio, va in `OUT_DIR/amdgpu-open-imports.txt`.

Com'è fatto:

- `build/amdgpu.rs`: la lista dei 376 oggetti è esplicita, ricavata con GNU
  make dal Makefile di amdgpu con la sola `CONFIG_DRM_AMDGPU` (DC, KFD, ACP,
  ISP, CIK, SI, ACPI, COMPAT, VGA_SWITCHEROO, HMM_MIRROR, PROC_FS,
  PERF_EVENTS spenti) e verificata a ogni build contro le 419 parole `.o`
  dei 15 Makefile di `amdgpu/`, `pm/` e `ras/`: 43 sono escluse con
  l'opzione che le selezionerebbe. `kbuild.rs` non è stato esteso. La
  feature `amdgpu-port` dipende da `nouveau-port`; `open_imports` è estratta
  da `build/nouveau.rs` e serve a entrambi.
- Due ritocchi per compilare: `-Wno-gnu` nel profilo `upstream_warnings` di
  `stela-c-build` (Linux lo passa a clang nel Makefile principale;
  `amdgv_sriovmsg.h` ha una union a dimensione variabile in mezzo a una
  struct) e `src/compat/linux/stela/amdgpu_census.h`, passato con
  `-include` al solo censimento, che dichiara le cinque funzioni che gli
  header di Linux dichiarano solo con opzioni che Stela non imposta
  (`irq_domain_create_linear`, `irq_domain_remove`, `irq_create_mapping`,
  `generic_handle_domain_irq` con `CONFIG_IRQ_DOMAIN`; `pfn_to_nid` con un
  modello di memoria), così contano come import aperti.
- Tutti i 376 oggetti compilano con il profilo di warning di Linux e
  `-Werror`. Nessun header `asm/` manca: quelli x86 di amdgpu sono sotto
  guardie `CONFIG_X86*`.
- Per il target UEFI (il triple cygwin con cui Stela compila il C di
  `stela.efi`) clang controlla i formati printf con le regole di Windows,
  dove `L` su una conversione intera è indefinito; amdgpu lo usa (`%010Lx`
  in `amdgpu_vm.c`, `%08Lx` in `vcn_v3_0.c`), nouveau e la classe DRM no. Il
  `vsprintf.c` di Stela lo legge come `ll`, come Linux. Il censimento su
  UEFI compila quindi con `-Wno-format`, mentre sull'host i formati restano
  controllati; il port vero dovrà scegliere tra questo e una patch che porti
  quei formati a `%ll`. Verificato il 2026-11-13: così i 376 oggetti
  compilano anche per il target UEFI e il censimento lì trova gli stessi
  servizi aperti dell'host.

Risultato: 91 servizi kernel aperti, di cui 11 trap che l'ambiente definisce
senza servire e 7 raggiunti solo attraverso oggetti dell'ambiente. Nove sono
gli stessi già aperti per nouveau; 82 sono propri di amdgpu.

| Gruppo | Servizi | Approccio |
|---|---|---|
| PCI e PCIe | 21 | adattatori in `compat/kernel/pci.c` |
| Sorgenti upstream fuori dalla classe DRM | 17 | compilare invariati |
| Libreria che l'ambiente non compila | 17 | compilare invariati, allargando lo sparse |
| Modello dei device | 11 | stub e adattatori |
| Interrupt | 8 | 2 adattatori, 6 trap |
| Memoria | 5 | adattatori, in parte trap |
| Tempo | 3 | adattatori |
| Stampa e riavvio | 2 | adattatori |
| Trap già noti a nouveau | 7 | nulla |

Nel dettaglio, con gli oggetti che li importano:

1. PCI e PCIe (21). Vettori MSI: `pci_alloc_irq_vectors`, `pci_irq_vector`,
   `pci_free_irq_vectors` (`amdgpu_irq`). Capability:
   `pci_find_ext_capability`, `pcie_capability_read_word`, `pcie_get_mps`,
   `pcie_get_speed_cap`, `pcie_get_width_cap`,
   `pci_enable_atomic_ops_to_root`. Resizable BAR: `pci_rebar_get_max_size`,
   `pci_rebar_bytes_to_size`, `pci_resize_resource`, `pci_bus_resource_n`
   (`amdgpu_device_resize_fb_bar`, che su questo PC allarga BAR0 da 256 MB a
   16 GB; amdgpu prosegue se il resize fallisce). Reset e sospensione:
   `pci_reset_function`, `pci_store_saved_state`, `pci_load_saved_state`,
   `pci_restore_msi_state`, `pci_clear_master`. `pci_get_class`
   (`smu_v14_0_0_ppt`, APU). `pci_wait_for_pending_transaction` e
   `pci_wake_from_d3` sono trap già definiti. In Linux:
   `drivers/pci/{pci,search,setup-res}.c` e `drivers/pci/msi/api.c`, nessuno
   nello sparse: sono adattatori di Stela sul suo PCI.
2. Sorgenti upstream fuori dalla classe DRM (17). Il buddy allocator per
   `amdgpu_vram_mgr`: `gpu_buddy_*` (6, `drivers/gpu/buddy.c`,
   `CONFIG_GPU_BUDDY`) e `drm_buddy_print` (`drm_buddy.c`); il
   sub-allocatore per `amdgpu_sa`: `drm_suballoc_*` (4, `drm_suballoc.c`);
   il client DRM per `amdgpu_amdkfd`: `drm_client_*` (3, `drm_client.c`); il
   modulo compagno di `amdgpu_xcp`: `amdgpu_xcp_*` (3,
   `amd/amdxcp/amdgpu_xcp_drv.c`). Da aggiungere a `DRM_SOURCES` in
   `build/native.rs`; `drivers/gpu/buddy.c` anche allo sparse.
3. Libreria che l'ambiente non compila (17). kfifo (5, `lib/kfifo.c`:
   `ras_core`, `ras_process`); mempool (6, `mm/mempool.c`: `ras_log_ring`);
   `down` e `up` (`kernel/locking/semaphore.c`: `amdgpu_device`); `gcd`
   (`lib/math/gcd.c`: `amdgpu_afmt`, `amdgpu_pll`); `bitmap_zalloc` e
   `bitmap_free` (`lib/bitmap-alloc.c`, nello sparse ma non compilato:
   `amdgpu_mes`); `sscanf` (`amdgpu_psp`, `amdgpu_gfx`; il `vsprintf.c` di
   Stela non lo fornisce). Dei file, solo `bitmap-alloc.c` è nello sparse.
4. Modello dei device (11). `device_create_file` e `device_remove_file`
   (10 oggetti: attributi sysfs di `amdgpu_pm`, `amdgpu_gmc`, `amdgpu_xgmi`,
   `amdgpu_gfx`, `amdgpu_sdma`, `amdgpu_vcn`, `amdgpu_jpeg`, `amdgpu_vpe`,
   `amdgpu_preempt_mgr`, `df_v3_6`), `devm_device_add_group`
   (`amdgpu_device`, `amdgpu_atombios`, `amdgpu_pm`), `devm_i2c_add_adapter`
   (9 oggetti: `amdgpu_i2c`, `smu_v11_0_i2c` e gli i2c dei `*_ppt`),
   `device_link_del` (`smu_v14_0_0_ppt`), `hwmon_device_register_with_groups`
   e `hwmon_device_unregister` (`amdgpu_pm`), `backlight_device_register` e
   `backlight_device_unregister` (`atombios_encoders`, DCE legacy),
   `device_link_add` e `platform_device_unregister` (trap, come per nouveau).
   Stela non ha sysfs né hwmon: stub che riescono; `devm_i2c_add_adapter`
   sopra l'`i2c.c` esistente.
5. Interrupt (8). `disable_irq` (`amdgpu_fence`) ed `enable_irq`
   (`amdgpu_irq`) sono adattatori in `irq.c`. Il dominio
   (`irq_domain_create_linear`, `irq_domain_remove`, `irq_create_mapping`,
   `generic_handle_domain_irq`, `handle_simple_irq`,
   `irq_set_chip_and_handler_name`, tutti da `amdgpu_irq`) lo creano solo
   `iceland_ih`, `tonga_ih`, `cz_ih` e `ih_v6_1`, non l'`ih_v7_0` di Navi
   48: trap.
6. Memoria (5). `page_is_ram` e `pfn_to_nid` (`gmc_v9_0`, flag di coerenza
   delle APU GFX9); `dma_map_resource` e `dma_unmap_resource`
   (`amdgpu_ttm`, `amdgpu_vram_mgr`: mapping P2P di VRAM altrui);
   `dma_addressing_limited` (`amdgpu_ttm`). Adattatori in `dma.c` e
   `page.c`; i primi quattro probabilmente trap per la 9070.
7. Tempo (3). `ktime_get_mono_fast_ns` (`amdgpu_device`),
   `ktime_get_real_seconds` (`amdgpu_ras_eeprom`, `amdgpu_umc`,
   `amdgpu_cper`, `amdgpu_ras_sys`, `ras_eeprom_fw`), `time64_to_tm`
   (`amdgpu_cper`). In Linux `kernel/time/{timekeeping,timeconv}.c`;
   adattatori in `timekeeping.c` e `time.c` di Stela.
8. Stampa e riavvio (2). `__printk_ratelimit` (`gmc_v7_0` … `gmc_v12_1`;
   `kernel/printk/printk.c`), `emergency_restart` (`amdgpu_device`;
   `kernel/reboot.c`).
9. Trap già noti a nouveau (7): `folio_clear_dirty_for_io`,
   `mm_get_unmapped_area`, `shmem_read_folio_gfp`,
   `shmem_read_mapping_page_gfp`, `shmem_truncate_range`, `shmem_writeout`,
   `stela_linux_reopen_unowned_path`.

Cosa dice sul lavoro kernel. L'incognita principale è misurata: 82 funzioni,
non sottosistemi interi. Nessun sottosistema grosso risulta necessario:
debugfs, perf, HMM e KFD sono fuori per configurazione, pm_runtime e il
firmware loader li fornisce già l'ambiente di nouveau, come workqueue,
timer, kthread, TTM, scheduler e dma-buf. Circa 34 servizi (gruppi 2 e 3)
si chiudono compilando una decina di file upstream invariati; una ventina
sono stub o trap; restano gli adattatori veri, concentrati su PCI (vettori
MSI, capability, Resizable BAR, reset) e sul modello dei device. Il
censimento misura solo gli import di link: la semantica a runtime
dell'ambiente di Stela (un proprietario alla volta, niente preemption) vale
per amdgpu quanto per nouveau e non è stata ancora provata con la 9070.

Passo 1, fatto il 2026-11-12: i gruppi 2 e 3 chiusi per quanto si compila
invariato. `third_party/linux.sparse` ha in più `drivers/gpu/buddy.c` (con
il `Makefile` di `drivers/gpu`, che lo compila come `buddy.o`),
`lib/kfifo.c`, `lib/math/gcd.c`, `kernel/locking/semaphore.c` e
`mm/mempool.c`; `autoconf.h` ha `CONFIG_DRM_BUDDY`, `CONFIG_GPU_BUDDY` e
`CONFIG_DRM_SUBALLOC_HELPER`, che amdgpu seleziona; l'ambiente comune di
`stela-linux` (`build/native.rs`) compila `kfifo.c`, `mempool.c`,
`semaphore.c`, `gcd.c` e `bitmap-alloc.c`; la classe DRM del crate GPU
(`build/native.rs`) compila `drm_buddy.c`, `drm_suballoc.c` e
`drivers/gpu/buddy.c`, quest'ultimo come unità di registrazione catturata
(`DRM_UNITS`, gruppo `gpubuddy`, ordinale 8: il suo `module_init` crea la
kmem_cache dei blocchi), con l'unità di nouveau spostata all'ordinale 9. Un
terzo flag nel profilo upstream, `-Wno-microsoft-anon-tag`, come
`CC_FLAGS_DIALECT` del Makefile di Linux: `mm/slab.h` include `struct
freelist_counters` come membro anonimo di `struct slab`. Nessun import nuovo
è arrivato attraverso i file aggiunti: `OPEN` è sceso da 91 a 64 (55 propri
di amdgpu). Dei 34 previsti ne restano 7, rimandati al passo 3 come
adattatore e trap: `sscanf` (serve un `vsscanf` nel `vsprintf.c` di Stela),
`drm_client_init`, `drm_client_register` e `drm_client_release` (raggiunti
solo con KFD) e i tre `amdgpu_xcp_*` (`amdxcp` è un platform driver con
initcall, per GPU a più partizioni).

Passo 2, fatto il 2026-11-13: gli adattatori PCI, 16 servizi forniti e 3
trap nuovi; `OPEN` da 64 a 48 (39 propri di amdgpu). Ogni funzione segue il
testo di `drivers/pci/` al pin (letto con `git show` dal checkout parziale,
fuori dallo sparse):

- `crates/stela-linux/src/compat/kernel/pci.c`: capability estese
  (`pci_find_ext_capability`), registri PCI Express
  (`pcie_capability_read_word` e `_dword`, con i registri che il tipo di
  porta non ha letti come zero e il Presence Detect dei downstream port),
  `pcie_get_mps`, `pcie_get_speed_cap`, `pcie_get_width_cap`,
  `pcie_get_supported_speeds`, `pci_cfg_space_size`, le finestre del bus
  (`pci_bus_resource_n`), la copia dello stato salvato
  (`pci_store_saved_state`, `pci_load_saved_state`), `pci_clear_master`,
  `pci_intx`, Resizable BAR (`pci_rebar_bytes_to_size`,
  `pci_rebar_get_possible_sizes`, `pci_rebar_get_max_size`) e
  `pci_enable_atomic_ops_to_root`. Trap: `pci_reset_function` (solo il
  metodo di reset `pci`, che SOC24 non sceglie mai), `pci_restore_msi_state`
  (solo MI300) e `pci_get_class` (solo APU e ATPX).
- `crates/stela-linux/src/compat/kernel/pci_host.c`: i vettori
  (`pci_alloc_irq_vectors`, `pci_irq_vector`, `pci_free_irq_vectors`) sul
  messaggio unico del proprietario o sulla linea INTx; l'attach ora imposta
  come il probe di Linux `pcie_flags_reg`, `cfg_size`, `supported_speeds` e
  `rebar_cap` (letture di configurazione in più per ogni funzione, anche per
  le GPU NVIDIA: il registro dei flag PCIe, la prima dword estesa, i due
  registri di capacità del link e la lista delle capability estese fino a
  ReBAR) e inizializza la lista delle finestre del bus.
- Scelte che cambiano il comportamento rispetto a Linux sul PC:
  `pci_resize_resource` rifiuta con `-ENOTSUPP`, come Linux quando il host
  bridge deve preservare l'assegnazione del firmware, perché le finestre le
  piazza il proprietario: BAR0 resta com'è (vedi «Da verificare»).
  `pci_enable_atomic_ops_to_root` non vede root port, perché il
  proprietario non presenta bridge: come sotto un hypervisor, le AtomicOp
  sono accese solo se il firmware le ha accese. Servono a KFD, che è spento,
  non alla grafica.
- I test hosted `linux_pci` e `linux_pci_host` coprono ognuna di queste
  funzioni.

| Gruppo | Aperti dopo il passo 2 |
|---|---|
| Trap PCI (reset, ripristino MSI, ricerca per classe, più due già definiti) | 5 |
| Modello dei device | 11 |
| Interrupt | 8 |
| DRM client (solo con KFD) e amdxcp | 6 |
| Memoria | 5 |
| Tempo | 3 |
| Stampa e riavvio | 2 |
| `sscanf` | 1 |
| Trap già noti a nouveau | 7 |

Passo 3, fatto il 2026-11-13: per ognuno dei 48 servizi il codice di amdgpu
è stato seguito fino a stabilire se un percorso sulla 9070 lo raggiunge. I
raggiunti sono forniti, gli altri sono trap che scrivono perché nessun
percorso sulla 9070 ci arriva. `OPEN` da 48 a 30: 29 trap e `sscanf`.

- Forniti nell'ambiente di `stela-linux` (`compat/kernel/`), ognuno sul testo
  di Linux al pin:
  - `pci.c`: `pci_wait_for_pending_transaction` (raggiunto alla rimozione e
    all'halt) e `pci_wake_from_d3` (raggiunto al probe: il runtime PM via
    BACO è attivo per la 9070), che sostituiscono due trap. Il wake è
    rifiutato con `-EINVAL` come per ogni device che non può svegliarsi:
    Stela non ne marca nessuno così.
  - `dma.c`: `dma_get_required_mask`, `dma_addressing_limited` (raggiunto
    in `amdgpu_ttm_init`: falso con i 44 bit di amdgpu, perché senza una
    maschera richiesta dalle ops vale la predefinita di Linux a 32 bit),
    `dma_map_resource` e `dma_unmap_resource` (MMIO via `dma_map_phys`).
  - `timekeeping.c`: `ktime_get_mono_fast_ns` (il monotono del
    proprietario) e `ktime_get_real_seconds` (la UTC del proprietario, o
    l'epoca); `time64_to_tm` è `kernel/time/timeconv.c` compilato invariato,
    aggiunto allo sparse. Test nuovo `linux_timekeeping`.
  - `printk.c`: `__printk_ratelimit` con lo stato condiviso di Linux (10
    messaggi ogni 5 s), raggiunto sui page fault della GPU.
  - `device.c`: `device_create_file`, `device_remove_file` e
    `devm_device_add_group` secondo `drivers/base/core.c`; senza
    `CONFIG_SYSFS` le funzioni sysfs sono gli stub inline di Linux, quindi
    l'attributo viene solo controllato e la rimozione del gruppo gestito
    registrata tra le azioni del driver.
  - `i2c.c`: `devm_i2c_add_adapter` (gli adattatori I2C dell'SMU 14).
  - `hwmon.c`, file nuovo GPL-2.0-only come `drivers/hwmon/hwmon.c`:
    `hwmon_device_register_with_groups` e `hwmon_device_unregister`; il
    device è registrato sotto il genitore con gruppi, dati e id da un IDA.
    Test nuovo `linux_hwmon`.
  - `irq.c`: `disable_irq` ed `enable_irq` come `kernel/irq/manage.c`, con
    una profondità per linea: il primo disable maschera la linea e attende
    i suoi gestori threaded, l'ultimo enable la smaschera; una consegna che
    trova la linea disabilitata non esegue gestori ed è ritrasmessa da
    `irq_work` all'enable, come la ritrasmissione software di Linux. Il
    proprietario dichiara che una consegna già in coda può arrivare dopo la
    maschera, quindi il controllo sta anche in `stela_linux_irq_run`.
    `IRQF_NO_AUTOEN` ora parte disabilitata fino a `enable_irq`. Diverso da
    Linux: `enable_irq` dorme sul mutex della linea, quindi vuole il
    contesto di processo, e lo verifica. Test nella fixture delle
    workqueue, che ha task e linee reali.
- Fornito nel crate GPU (`src/compat/linux/kernel/amdgpu.c`, nuovo):
  `amdgpu_xcp_drv_release`, che amdgpu chiama all'uscita e che in amdxcp
  libera i device di partizione allocati, qui nessuno.
- Trap nuovi, ciascuno con il motivo: `page_is_ram` e `pfn_to_nid` (solo il
  GMC 9 delle APU), `emergency_restart` (solo Vega20 con firmware vecchio
  dopo un errore RAS fatale), `backlight_device_register` e `_unregister`
  (solo gli encoder DCE legacy; i DCN passano da DC), `device_link_del`
  (solo il link USB-C delle APU), il dominio IRQ (`irq_domain_create_linear`,
  `irq_domain_remove`, `irq_create_mapping`, `generic_handle_domain_irq`,
  `handle_simple_irq`, `irq_set_chip_and_handler_name`: solo gli IH di
  generazione VI e 6.1 lo creano), il DRM client (solo KFD) e
  `amdgpu_xcp_drm_dev_alloc` e `_free` (solo GPU partizionate: GC 9.4.3 e
  12.1).
- `sscanf` resta aperto di proposito. amdgpu lo usa solo in store sysfs e
  nel parametro `disable_cu`, vuoto di default, ma la libc di compatibilità
  di Mesa (`stela-driver-gpu/src/compat/libc/stdio.h`) definisce già
  `sscanf` nella stessa immagine: un `sscanf` dell'ambiente Linux, anche
  come trap, si scontrerebbe. Quando amdgpu verrà linkato servono la
  ridenominazione di quello di Mesa in `stela_libc_sscanf`, come già per i
  quattro nomi printf, e un `vsscanf` nel `vsprintf.c` di Stela.

| Gruppo | Aperti dopo il passo 3 |
|---|---|
| Trap PCI | 3 |
| Trap DRM client e partizioni amdxcp | 5 |
| Trap dominio IRQ | 6 |
| Trap del modello dei device | 5 |
| Trap memoria | 2 |
| Trap riavvio d'emergenza | 1 |
| Trap già noti a nouveau | 7 |
| `sscanf`, aperto di proposito | 1 |

Prossimo passo proposto, in ordine:

1. Gruppi 2 e 3: allargare `third_party/linux.sparse` e `DRM_SOURCES`,
   compilare i file invariati, far scendere `OPEN`. Fatto il 2026-11-12.
2. Adattatori PCI in `compat/kernel/pci.c`, con `pci_resize_resource` che
   rifiuta (BAR0 resta a 256 MB finché Stela non sa riallocare). Fatto il
   2026-11-13.
3. Stub e adattatori dei gruppi 4–8; trap per i gruppi 5 (dominio) e 6, per
   `drm_client` e `amdxcp`. Fatto il 2026-11-13, tranne `vsscanf` (vedi
   sopra).
4. Il port vero: `autoconf.h` con `CONFIG_DRM_AMDGPU`, amdgpu linkato in
   `stela.efi` (con la ridenominazione di `sscanf`), le ricette firmware
   (strato 2) e il boot (strato 3, `claim` e il reset senza FLR: MODE1 via
   SMU), verso la tappa 2 del piano. Da qui serve il PC: il primo probe
   della 9070 si vede solo avviando Stela. Fatto il 2026-11-13, vedi sotto;
   il probe resta da vedere sul PC.

Passo 4, fatto il 2026-11-13 in due parti: compilato e testato qui, ma
sulla 9070 non è ancora stato eseguito niente.

- **4a, amdgpu linkato in `stela.efi`** (commit `1086695b2`).
  - `crates/stela-driver-gpu/build/image.rs` compone l'immagine di
    registrazione `stela_linux_gpu_image`: la coorte comune, le unità DRM e
    l'unità di registrazione di ogni port, in ordine globale. nouveau resta
    all'ordinale 9 e `amdgpu_drv` prende il 10, quindi `amdgpu_init`
    registra il driver PCI dopo quello di nouveau. Ogni port tiene la sua
    unità nel proprio archivio (`stela-nouveau-nvkm`, `stela-amdgpu`), e su
    UEFI gli archivi si linkano accanto all'immagine. La avvia
    `gpu::linux::start`, che prima era `gpu::nouveau::start`.
  - Feature: `amdgpu-port` di `stela-driver-gpu` linka amdgpu,
    `stela-drivers` la inoltra e la nuova `amdgpu` di `stela-boot` la
    accende. `autoconf.h` definisce `CONFIG_DRM_AMDGPU`.
  - `sscanf`: c'è il nuovo `compat/kernel/vsscanf.c`, sul testo di
    `lib/vsprintf.c`, con il test `linux_vsscanf`, e `kstrtox.c` ha il
    fixup della base di `lib/kstrtox.c`. La libc di Mesa dichiara il suo
    `sscanf` come `stela_libc_sscanf`, verificato con `nm`. `OPEN` passa da
    30 a 29 servizi, tutti trap.
  - `stela.efi` di debug: 281 MB con amdgpu, che ne contiene le stringhe, e
    157 MB senza, come prima.
- **4b, claim e firmware della 9070** (commit `7725e30bf` e `ea7a34286`).
  - `cold.rs`: il reset al claim dipende dal vendor. NVIDIA ha
    `Reset::Function`, cioè l'FLR di prima. AMD ha `Reset::Driver`, cioè
    nessun reset al claim. amdgpu resetta da sé (MODE1 via SMU) quando il
    sOS del PSP è già vivo (`soc24_need_reset_on_init`); altrimenti prende
    la scheda come l'ha lasciata il firmware, come fa Linux a ogni avvio.
    Il bus master resta spento finché amdgpu non lo accende, e da lì il
    dominio DMA privato limita ciò che la GPU raggiunge.
  - `crates/stela-boot/src/engine/gpu.rs` ha una `Driver` per vendor. Per
    amdgpu i registri stanno al BAR 5 (`amdgpu_device_init`) e il limite
    DMA è a 44 bit: GC 12.0.1 prende quel ramo di `gmc_v12_0_sw_init`. Il
    chip viene dall'ID PCI (`1002:7550` è `navi48`), non da un registro,
    perché amdgpu legge il resto dalla tabella IP discovery.
  - Se lo schermo del firmware sta sulla 9070, il claim la rifiuta e il log
    dice di spostare il monitor sulla scheda madre. amdgpu è compilato
    senza DC e lascerebbe lo schermo nero.
  - Il log scrive `GPU <indirizzo>: amdgpu bound it` oppure
    `did not bind it`. Solo le GPU di nouveau vanno al device Metal, quindi
    un'immagine con `nvk` e `amdgpu` insieme non scambia la 9070 per una
    NVIDIA mancata.
  - Firmware (`crates/stela-loader/src/host_firmware/recipes.rs`, `AMDGPU`):
    11 file da linux-firmware `20261019` (`a2dcb924…`) per GC 12.0.1, PSP e
    SMU 14.0.3, SDMA 7.0.1 e VCN 5.0.0, più `LICENSES/LICENSE.amdgpu`, in
    tutto 2,8 MB. MES usa l'immagine unica, perché `amdgpu_uni_mes` è
    attivo di default. TOC, DMCUB e le versioni "kicker" (solo `1002:7551`
    rev C8) non vengono chieste. Il bundle della chiavetta passa da 64 a 75
    file. Le funzioni sono `amd_chip` e `require_amdgpu`, e la schermata di
    boot ha i messaggi per AMD.
  - xtask: `cargo xtask boot build --release --amdgpu` costruisce
    l'immagine con amdgpu e senza NVK.

## Prova sul PC (tappa 2)

Scopo: vedere nel log di Stela il primo probe di amdgpu sulla 9070. Questa
immagine non dà a macOS un device Metal, quindi macOS va avviato in
single-user: quell'avvio passa comunque dal claim della GPU. Serve una
chiavetta Stela già preparata, con macOS installato.

1. Monitor sulle uscite della scheda madre, cioè sulla iGPU. Nel BIOS la
   prima uscita video va sulla grafica integrata, di solito «Settings → IO
   Ports → Initial Display Output → IGD Video» sulle Gigabyte AM5, con la
   grafica integrata attiva. Se lo schermo del firmware resta sulla 9070,
   il claim la rifiuta.
2. L'immagine:

   ```sh
   export PATH="$HOME/.rustup/toolchains/1.99.1-x86_64-unknown-linux-gnu/bin:$PATH"
   cargo xtask boot build --release --amdgpu
   ```

   Stampa il percorso di `stela-deploy.efi`.
3. La CLI di questo branch: `cargo build --release -p stela-cli` dà
   `target/release/stela`. Il suo bundle comprende gli 11 file di amdgpu.
4. Si installa l'immagine sulla chiavetta, dove `USB` è la partizione EFI
   della chiavetta montata. L'aggiornamento completa anche il bundle, quindi
   scarica e copia i file di amdgpu:

   ```sh
   target/release/stela setup update stela --usb USB --efi target/boot/x86_64-unknown-uefi/release/stela-deploy.efi
   ```

   `stela setup status --usb USB` dice se il bundle è completo, e
   `stela setup host-firmware fetch --usb USB` lo completa da solo.

5. In `EFI/stela/choices.cfg` della chiavetta si duplica la riga della
   scelta macOS (nome, tab, opzioni) e si aggiunge `--single-user` in fondo
   alla copia. All'avvio di Stela si sceglie quella voce.
6. Dopo l'avvio si legge il log con
   `target/release/stela setup boot-log --usb USB`, oppure dal file
   `EFI/stela/boot.log`. Le righe da cercare:
   - `HOST FIRMWARE BUNDLE USE amdgpu/…`: gli 11 file letti;
   - `GPU 0000:03:00.0 …`: il claim, o il motivo del rifiuto;
   - `LINUX <n> amdgpu 0000:03:00.0: …`: il probe di amdgpu;
   - `GPU 0000:03:00.0: amdgpu bound it`, oppure `did not bind it`;
   - `GPU START claim_ms=… probe_ms=…`.

## Controlli sul PC (Linux)

```sh
sudo lspci -nnvv -d 1002:
sudo dmesg | grep -i amdgpu
```

Nel primo cercare: gli ID dei due dispositivi, `FLReset+` o `FLReset-` in
`DevCap`, le dimensioni dei BAR, MSI e MSI-X. Nel secondo: le versioni degli
IP e i firmware caricati. Annotare anche dove è collegato il monitor e
l'impostazione del BIOS per la grafica integrata.

Eseguiti il 2026-11-12 senza `sudo` (chiede la password): `lspci -nn`,
sysfs (`reset_method`, `ip_discovery`, `fw_version`, `resource`, stato dei
connettori DRM) e `journalctl -k`. I risultati sono in «Verificato sul PC».
Manca solo `DevCap` (bit `FLReset`), che `lspci` mostra soltanto da root.

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

## Ambiente di build sul PC (2026-11-12)

- Rust: `rustup` (snap, `/snap/bin/rustup`) ha installato la 1.99.1 di
  `rust-toolchain.toml` con `x86_64-unknown-linux-musl`,
  `x86_64-unknown-uefi`, rustfmt, clippy e llvm-tools. Nel PATH però `cargo`
  e `rustc` sono i pacchetti Ubuntu 1.93.1 in `/usr/bin`, che ignorano
  `rust-toolchain.toml`; i proxy di rustup stanno in `/snap/bin`, dopo
  `/usr/bin`, e `~/.cargo/bin` non li ha. Dentro `rustup run 1.99.1` il
  comando `which rustc` dà ancora `/usr/bin/rustc`, e `Toolchain::discover`
  cerca `rustc` nel PATH. Ogni comando va quindi lanciato con il toolchain
  davanti nel PATH:

  ```sh
  export PATH="$HOME/.rustup/toolchains/1.99.1-x86_64-unknown-linux-gnu/bin:$PATH"
  cargo xtask gpu fetch
  ```

- Clang 21.0.0 (build swiftlang in `~/.local/share/swiftly/bin`: `clang`,
  `clang++`, `clang-21`, `ld.lld`).
- Python 3.14.4 di sistema; il fetch crea il venv in `.tools/gpu/generators`
  con meson, ninja, pkgconf, Mako, packaging e PyYAML pinnati, e installa
  bindgen-cli e cbindgen in `.tools/gpu/rust`.
- Prima del fetch i submodule `third_party/{linux,mesa,llvm,libdrm,xnu}`
  erano vuoti.

## Stato

- 2026-11-12, sessione cloud: nessuna build eseguita, nessun test lanciato,
  nessun file modificato a parte questo documento.
- 2026-11-12, sessione locale sul PC:
  - branch `claude/amd-radeon-rx9070-rdna4-y12vd2` creato in locale da
    `origin`; è `main` più questo documento;
  - controlli hardware eseguiti (vedi «Verificato sul PC»);
  - toolchain Rust 1.99.1 installata da rustup (vedi «Ambiente di build»);
  - `/drivers/gpu/drm/amd/` aggiunto a `third_party/linux.sparse` (passo 3
    del censimento) prima del primo fetch, così il fetch è uno solo;
  - `cargo xtask gpu fetch` riuscito in due minuti e mezzo: i cinque
    submodule sono sul branch `stela` al loro pin, Linux con le 200 patch
    applicate e Mesa con le 26; `third_party/linux/drivers/gpu/drm/amd/` è
    nel checkout (2756 file `.c` e `.h`, 538 MB, `asic_reg` compreso);
    `.tools/gpu` ha il venv dei generatori e bindgen/cbindgen. `third_party`
    pesa 1 GB;
  - censimento amdgpu eseguito, vedi «Censimento amdgpu: risultati». File
    nuovi: `crates/stela-driver-gpu/build/amdgpu.rs` e
    `crates/stela-driver-gpu/src/compat/linux/stela/amdgpu_census.h`.
    Modificati: `crates/stela-driver-gpu/Cargo.toml` (feature `amdgpu-port`),
    `build.rs`, `build/nouveau.rs` (`open_imports` condivisa),
    `crates/stela-c-build/src/linux_build.rs` (`-Wno-gnu`),
    `third_party/linux.sparse`. `cargo fmt --check` e clippy puliti sulla
    lane host (`amdgpu-port`) e sulla lane UEFI (`metal-linked,nouveau-port`).
    La lane host di `cargo xtask check` usa `--all-features`, quindi ora
    compila anche amdgpu (meno di due minuti in più). Committato in tre
    commit;
  - passo 1 eseguito (vedi «Censimento amdgpu: risultati»): `OPEN` a 64.
    Modificati `third_party/linux.sparse`,
    `crates/stela-linux/src/compat/generated/autoconf.h`,
    `crates/stela-linux/build/native.rs`,
    `crates/stela-driver-gpu/build/{native,nouveau,amdgpu}.rs` e
    `crates/stela-c-build/src/linux_build.rs`. Build con `amdgpu-port`
    riuscita, clippy pulito sulla lane host e su quella UEFI;
    `cargo test -p stela-linux -p stela-driver-gpu --all-features`: 735
    test superati, nessuno fallito. Committato in tre commit.
- 2026-11-13, sessione locale sul PC (riavviato nel mezzo; branch e
  sorgenti intatti):
  - passo 2 eseguito (vedi «Censimento amdgpu: risultati»): `OPEN` a 48.
    Modificati `crates/stela-linux/src/compat/kernel/{pci,pci_host}.c`,
    `crates/stela-linux/src/compat/stela/pci.h` e
    `crates/stela-driver-gpu/build/amdgpu.rs`. Build con `amdgpu-port` riuscita sull'host e per il target UEFI (`x86_64-unknown-uefi`), dove il censimento trova gli stessi 48 servizi; clippy pulito sulla lane host e su quella UEFI; `cargo test -p stela-linux -p stela-driver-gpu --all-features`: 735 test superati, nessuno fallito, `linux_pci` e `linux_pci_host` compresi. Committato in tre commit.
  - passo 3 eseguito (vedi «Censimento amdgpu: risultati»): `OPEN` a 30, 29
    trap e `sscanf`. Build con `amdgpu-port` riuscita sull'host e per il target UEFI, dove il censimento trova gli stessi 30 servizi; `cargo test -p stela-linux -p stela-driver-gpu --all-features`: 737 test superati, nessuno fallito, compresi i nuovi `linux_hwmon` e `linux_timekeeping` e la fixture delle workqueue con `disable_irq` ed `enable_irq`; clippy pulito sulla lane host e su quella UEFI. Committato in tre commit di codice e uno di documentazione.
  - passo 4 eseguito (vedi «Censimento amdgpu: risultati»), in due parti:
    amdgpu linkato in `stela.efi` (4a), poi il claim della 9070 e le sue
    ricette firmware (4b). Test host superati: 738 su `stela-linux` e
    `stela-driver-gpu` dopo 4a; dopo 4b, 604 su `stela-driver-gpu`, 642
    su `stela-boot-core`, 342 su `stela-loader` e 250 su `stela-cli`, più
    quelli di xtask, `stela-dev` e `stela-gpu-maintenance`. Clippy pulito
    sull'immagine UEFI con e senza `amdgpu`, sulle lane UEFI di
    `stela-boot-core` e `stela-driver-gpu` e sui crate host toccati. Le
    due immagini UEFI linkano. La lane `nvk` non è stata compilata, perché
    su questo PC Mesa non è costruito (`cargo xtask gpu nvk`): il codice
    che solo lei compila è stato riletto, non compilato. Committato in tre
    commit di codice e uno di documentazione.
- Decisione presa il 2026-11-12: censimento prima dell'upstream, poi i
  passi 1, 2 e 3. Il passo 4 è fatto; il prossimo è la prova sul PC
  (vedi «Prova sul PC (tappa 2)»). Sentire l'upstream resta consigliato
  prima degli strati che costano mesi (RADV, Metal, display).
