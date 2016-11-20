# 최신 ARM 리눅스 커널 4.x 분석

## 커뮤니티: IAMROOT 12차
- [www.iamroot.org][#iamroot] | IAMROOT 홈페이지
- [jake.dothome.co.kr][#moonc] | 문c 블로그
[#iamroot]: http://www.iamroot.org
[#moonc]: http://jake.dothome.co.kr

## History

첫 모임: 2015년 4월 25일

### 1주차
2015.04.25, NIPA (5x명)
- Orientation

### 2주차
- 2015.05.02, 단국대 죽전 (4x명)
- 리눅스 커널 내부구조 (처음 ~ 86page)

### 3주차
- 2015.05.09, 단국대 죽전 (3x명)
- 리눅스 커널 내부구조 (86 ~ 124page)

### 4주차
- 2015.05.16, 단국대 죽전 (2x명)
- 리눅스 커널 내부구조 (124 ~ 197page)

### 5주차
- 2015.05.23, 단국대 죽전(20명)
- 리눅스 커널 내부구조 (197 ~ 247page)

### 6주차
- 2015.05.30, 단국대 죽전 (21명)
- ARM System Developer's Guide (처음 ~ 364page)

### 7주차
- 2015.06.06, 단국대 죽전 (13명)
- ARM System Developer's Guide (364 ~ 517page)

### 8주차
- 2015.06.13, (메르스로 인한 쉼)

### 9주차
- 2015.06.20, 단국대 죽전 (14명)
- ARM System Developer's Guide (517 ~ 끝)

### 10주차
- 2015.06.27, 단국대 죽전 (16명)
- 분석 환경설정

### 11주차
- 2015.07.04, 단국대 죽전 (1x명) 
- zImage 생성과정

### 12주차
- 2015.07.11, 단국대 죽전 (15명)
- compressed/head.S - (1)

### 13주차
- 2015.07.18, 단국대 죽전 (1x명) 
- compressed/head.S - (2)

### 14주차
- 2015.07.25, 단국대 죽전 (10명) 
- compressed/head.S - (3)

### 15주차
- 2015.08.01, (휴가)

### 16주차
- 2015.08.08, 단국대 죽전 (8명) 
- compressed/head.S - (4)

### 17주차
- 2015.08.15, 단국대 죽전 (12명) 
- compressed/head.S - (5)

### 18주차
- 2015.08.22, 단국대 죽전 (11명) 
- Barrier 스터디

### 19주차
- 2015.08.29, 단국대 죽전 (13명) 
- Coretex A Programmers Guide (8.1 ~ 8.4)

### 20주차
- 2015.09.05, 단국대 죽전 (9명) 
- Coretex A Programmers Guide (8.4 ~ 8.11)

### 21주차
- 2015.09.12, 단국대 죽전 (11명) 
- Coretex A Programmers Guide (9.1 ~ 9.6)

### 22주차
- 2015.09.19, 단국대 죽전 (11명) 
- Coretex A Programmers Guide (9.7 ~ 10.1)

### 23주차
- 2015.09.23, (추석 명절)

### 24주차
- 2015.10.03, 단국대 죽전 (13명)
- Coretex A Programmers Guide (10.2 ~ 10.3)

### 25주차
- 2015.10.10, 단국대 죽전 (9명) 
- compressed/head.S - (6)

### 26주차
- 2015.10.17, 단국대 죽전 (11명) 
- compressed/head.S - (7)

### 27주차
- 2015.10.24, 단국대 죽전 (9명) 
- compressed/head.S - (8)

### 28주차
- 2015.10.31, 단국대 죽전 (10명) 
- kernel/head.S - (1)

### 29주차
- 2015.11.07, 단국대 죽전 (9명) 
- kernel/head.S - (2)

### 30주차
- 2015.11.14, 단국대 죽전 (6명) 
- kernel/head.S - (3)

### 31주차
- 2015.11.21, 단국대 죽전 (7명) 
- kernel/head.S - (4)

### 32주차
- 2015.11.28, 단국대 죽전 (7명) 
- kernel/head.S - (5)
- start_kernel() 시작

### 33주차
- 2015.12.05, 단국대 죽전 (6명) 
- smp_setup_processor_id()
- debug_objects_early_init()

### 34주차
- 2015.12.12, 단국대 죽전 (9명) 
- debug_objects_early_init()
- boot_init_stack_canary() - (1)

### 35주차
- 2015.12.19, 단국대 죽전 (7명) 
- boot_init_stack_canary() - (2)
- cgroup_init_early()

### 36주차
- 2015.12.26, (성탄연휴)   

### 37주차
- 2016.01.02, (신정연휴)   

### 38주차
- 2016.01.09, 단국대 죽전 (9명) 
- boot_cpu_init()
page_address_init()

### 39주차
- 2016.01.16, 단국대 죽전 (6명) 
- setup_arch()->setup_processor()

### 40주차
- 2016.01.23, 단국대 죽전 (10명) 
- setup_arch()->setup_machine_fdt()  

### 41주차
- 2016.01.30, 단국대 죽전 (6명) 
- spin_lock - (1)

### 42주차
- 2016.02.06, (구정연휴)

### 43주차
- 2016.02.13, 토즈 선릉점 (11명) 
- spin_lock - (2)

### 44주차
- 2016.02.20, 토즈 강남타워점 (13명) 
- mutex - (1)

### 45주차
- 2016.02.27, 토즈 강남타워점 (11명) 
- mutex - (2)

### 46주차
- 2016.03.05, 토즈 선릉점 (12명) 
- mutex - (3)

### 47주차
- 2016.03.12, 토즈 선릉점 (9명) 
- mutex - (4)
- memblock - (1)

### 48주차
- 2016.03.19, 토즈 선릉점 (9명) 
- memblock - (2)

### 49주차
- 2016.03.26, 토즈 선릉점 (11명) 
- memblock - (3)
- setup_arch()->setup_machine_tags() ~

### 50주차
- 2016.04.02, 토즈 선릉점 (x명)
- setup_arch()-setup_machine_tags()

### 51차
- 2016.04.09, 토즈 선릉점 (x명)
- setup_arch()->parse_early_param()

### 52주차
- 2016.04.16, 토즈 선릉점 (x명)
- setup_arch()->early_paging_init()
- setup_arch()->setup_dma_zone()
- setup_arch()->sanity_check_meminfo()

### 53주차
- 2016.04.23, 토즈 선릉점 (x명)
- setup_arch()->arm_memblock_init()

### 54주차
- 2016.04.30, 토즈 선릉점 (x명)
- setup_arch()->paging_init()->build_mem_type_table()
- setup_arch()->paging_init()->prepare_page_table() ~

### 55주차
- 2016.05.07, 토즈 선릉점 (x명)
- setup_arch()->paging_init()->prepare_page_table()
- setup_arch()->map_lowmem() 

### 56주차
- 2016.05.14, 토즈 선릉점 (x명)
- setup_arch()->paging_init()->dma_contiguous_remap()

### 57주차
- 2016.05.21, 토즈 선릉점 (x명)
- setup_arch()->paging_init()->devicemaps_init()

### 58주차
- 2016.05.28, 토즈 선릉점 (9명, 김종철, 박진영, 문영일, 양유석, 유계성, 윤창호, 조현철, 최영민, 한상종)
- setup_arch()->paging_init()->kmap_init()
- setup_arch()->paging_init()->tcm_init()
- setup_arch()->paging_init()->bootmem_init() ~

### 59주차
- 2016.06.04, 토즈 선릉점 (7명, 문영일, 양유석, 유계성, 윤창호, 조현철, 최영민, 한상종)
- setup_arch()->paging_init()->bootmem_init()->sparse()

### 60주차
- 2016.05.14, 토즈 선릉점 (10명, 권경환, 김종철, 문영일, 박진영, 양유석, 유계성, 윤창호, 조현철, 최영민, 한상종)
- setup_arch()->paging_init()->bootmem_init()->zone_sizes_init()
- setup_arch()->paging_init()->bootmem_init()->zone_sizes_init()->free_area_init_node() ~

### 61주차
- 2016.06.18, 토즈 선릉점 (9명, 권경환, 김종철, 문영일, 양유석, 유계성, 윤창호, 조현철, 최영민, 한상종)
- setup_arch()->paging_init()->bootmem_init()->zone_sizes_init()->free_area_init_node() ~

### 62주차
- 2016.06.25, 토즈 선릉점 (11명, 권경환, 김종철, 문영일, 박진영, 양유석, 유계성, 윤창호, 조현철, 최영민, 한대근, 한상종)
- setup_arch()->paging_init()->bootmem_init()->zone_sizes_init()->free_area_init_node()

### 63주차
- 2016.07.02, 토즈 선릉점 (9명, 권경환, 김종철, 문영일, 박진영, 유계성, 윤창호, 조현철, 한대근, 한상종)
- setup_arch()->paging_init()->__flush_dcache_page()
- setup_arch()->request_standard_resource()
- setup_arch()->unflatten_device_tree()

### 64주차
- 2016.07.09, 토즈 선릉점 (9명, 권경환, 김종철, 문영일, 박진영, 유계성, 윤창호, 조현철, 최영민, 한상종)
- setup_arch()->arm_dt_init_cpu_maps()
- setup_arch()->psci_init()
- setup_arch()->smp_init_cpus()
- setup_arch()->smp_build_mpidr_hash()
- setup_arch()->hyp_mode_check()
- setup_arch()->reserve_crashkernel()
- setup_arch()->mdesc->init_early()

### 65주차
- 2016.07.16, 토즈 선릉점 (10명, 권경환, 김종철, 문영일, 박진영, 양유석, 유계성, 윤창호, 조현철, 최영민, 한상종)
- mm_init_cpumask()
- setup_command_line()
- setup_nr_cpu_ids()
- setup_per_cpu_areas() ~

### 66주차
- 2016.07.23, 토즈 선릉점 (7명, 권경환, 문영일, 박진영, 양유석, 윤창호, 조현철, 한상종)
- setup_per_cpu_areas() ~

### 67주차
- 2016.07.30, 토즈 선릉점 (7명, 권경환, 김종철, 문영일, 유계성, 윤창호, 조현철, 한상종)
- setup_per_cpu_areas()
- per_cpu API들 ~

### 68주차
- 2016.08.06, 토즈 선릉점 (10명, 권경환, 김종철, 문영일, 박진영, 양유석, 유계성, 윤창호, 조현철, 최영민, 한상종)
- per_cpu API들 ~

### 69주차
- 2016.08.13, 강남 윙스터디2 (7명, 김종철, 문영일, 박진영, 윤창호, 조현철, 최영민, 한상종)
- per_cpu API들
- smp_prepare_boot_cpu()
- build_all_zonelists()

### 70주차
- 2016.08.20, 강남 상상플러스 (9명, 권경환, 김종철, 문영일, 양유석, 유계성, 윤창호, 조현철, 최영민, 한상종)
 - page_alloc_init()
 - parse_args()
 - jump_label_init() ~

### 71주차
- 2016.08.27, 선릉 Kosslab (9명, 권경환, 김종철, 문영일, 박진영, 양유석, 유계성, 윤창호, 조현철, 최영민)
 - jump_label_init()

### 72주차
- 2016.09.03, 토즈 선릉점 (9명, 권경환, 김종철, 문영일, 박진영, 양유석, 유계성, 윤창호, 조현철, 최영민)
 - setup_log_buf()
 - pidhash_init()
 - vfs_caches_init_early()
 - sort_main_extable()
 - trap_init()

### 73주차
- 2016.09.10, 선릉 Kosslab (8명, 권경환, 문영일, 박진영, 유계성, 윤창호, 조현철, 최영민, 한상종)
 - mm_init() -> page_ext_init_flatmem()
 
### 74주차
- 2016.09.17, (추석 명절)
 
### 75주차
- 2016.09.24, 선릉 Kosslab (8명, 김종철, 문영일, 박진영, 양유석, 윤창호, 조현철, 최영민, 한상종)
 - mm_init() -> mem_init() ~

### 76주차
- 2016.10.01, 토즈 선릉점 (10명, 권경환, 김민호, 김종철, 문영일, 유계성, 윤창호, 임채훈, 조현철, 최영민, 한상종)
 - mm_init() -> mem_init() ~
 
### 77주차
- 2016.10.08, Kosslab (12명, 권경환, 김미르, 김민호, 김종철, 문영일, 박진영, 양유석, 윤창호, 임채훈, 조현철, 최영민, 한상종)
 - mm_init() -> mem_init()
 - 버디 할당/해제 ~  

### 78주차
- 2016.10.15, (야외 행사)
 
### 79주차
- 2016.10.22, Kosslab (9명, 권경환, 김종철, 문영일, 양유석, 윤창호, 임채훈, 조현철, 최영민, 한상종)
 - 버디 할당/해제

### 80주차
- 2016.10.29, 토즈 선릉점 (10명, 김미르, 권경환, 문영일, 박진영, 양유석, 유계성, 윤창호, 임채훈, 조현철, 최영민)
 - zonned allocator ~  

### 81주차
- 2016.11.05, Kosslab (8명, 김민호, 김종철, 문영일, 박진영, 윤창호, 임채훈, 조현철, 최영민)
 - zonned allocator ~  
 
### 82주차
- 2016.11.12, 토즈 선릉점 (9명, 권경환, 김종철, 문영일, 양유석, 유계성, 윤창호, 임채훈, 조현철, 최영민)
 - zonned allocator (Direct-Compaction) ~  
 
### 83주차
- 2016.11.19, Kosslab (11명, 권경환, 김민호, 김종철, 문영일, 양유석, 유계성, 윤창호, 임채훈, 조현철, 최영민, 한상종)
 - zonned allocator (Direct-Compaction) ~  
