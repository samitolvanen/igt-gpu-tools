panthor_progs = [
	'panthor_devfreq',
	'panthor_gem',
	'panthor_group',
	'panthor_pm',
	'panthor_query',
	'panthor_scheduler',
	'panthor_sync',
	'panthor_vm',
]

foreach prog : panthor_progs
	test_executables += executable(prog, prog + '.c',
				       dependencies : test_deps,
				       install_dir : panthordir,
				       install_rpath : panthor_rpathdir,
				       install : true)
	test_list += join_paths('panthor', prog)
endforeach
