Build {
	Units = function ()
		local exampleclient = Program {
			Name = "exampleclient",
			Sources = {
				"cpp/example-client.cpp",
				"cpp/http-bridge.cpp",
			},
			Includes = {
				"cpp/flatbuffers/include",
			},
			Libs = {
				{ "Ws2_32.lib"; Config = "win*" },
			},
		}

		Default(exampleclient)
	end,

	Env = {
		CXXOPTS = {
			"/W3",
			--"/analyze",
			{ "/EHsc"; Config = "win*" },
			{ "/O2"; Config = "*-vs2013-release" },
		},
		GENERATE_PDB = {
			{ "0"; Config = "*-vs2013-release" },
			{ "1"; Config = { "*-vs2013-debug", "*-vs2013-production" } },
		},
	},

	Configs = {
		Config {
			Name = "macosx-gcc",
			DefaultOnHost = "macosx",
			Tools = { "gcc" },
		},
		Config {
			Name = "linux-gcc",
			DefaultOnHost = "linux",
			Tools = { "gcc" },
		},
		Config {
			Name = "freebsd-clang",
			DefaultOnHost = "freebsd",
			Tools = { "clang" },
		},
		Config {
			Name = "win32-msvc",
			SupportedHosts = { "windows" },
			Tools = { { "msvc-vs2013"; TargetArch = "x86" } },
		},
		Config {
			Name = "win64-msvc",
			DefaultOnHost = "windows",
			Tools = { { "msvc-vs2013"; TargetArch = "x64" } },
		},
	},

	IdeGenerationHints = {
		Msvc = {
			-- Remap config names to MSVC platform names (affects things like header scanning & debugging)
			PlatformMappings = {
				['win64-vs2013'] = 'x64',
				['win32-vs2013'] = 'Win32',
			},
			-- Remap variant names to MSVC friendly names
			VariantMappings = {
				['release']    = 'Release',
				['debug']      = 'Debug',
				['production'] = 'Production',
			},
		},

		-- Override output directory for sln/vcxproj files.
		MsvcSolutionDir = 'vs2013',
	}
}
