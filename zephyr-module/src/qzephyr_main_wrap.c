/* Zephyr enters the application through `int main(void)`: the kernel's
 * bg_thread_main() calls main with no arguments.  A Qt application defines
 * `int main(int argc, char **argv)` and QCoreApplication reads (and
 * rewrites: it strips `-platform` and friends from argv) whatever x0/x1
 * happened to hold -- on the AM62P that was a pointer into .rodata and the
 * first write faulted.  The link wraps `main` (-Wl,--wrap=main in
 * zephyr/CMakeLists.txt) so the kernel's call lands here and the Qt main
 * gets a real argument vector. */

#include <stdlib.h>
#include <string.h>

extern int __real_main(int argc, char **argv);

int __wrap_main(void)
{
	static char arg0[] = "zephyr";
	static char *argv[] = { arg0, 0 };

	/* PCRE2's JIT (QRegularExpression, hence the QML engine) writes machine
	 * code into heap memory and jumps to it.  On an MMU target Zephyr's
	 * heap is not executable, so that jump is an instruction abort; Qt
	 * honours this variable and falls back to the interpreter. */
	setenv("QT_ENABLE_REGEXP_JIT", "0", 0);
#ifdef CONFIG_QT_DEBUG_LOG
	/* No shell to set environment variables on the board: turn on the Qt
	 * Quick / RHI diagnostics the same way QSG_INFO=1 would. */
	setenv("QSG_INFO", "1", 1);
	setenv("QT_LOGGING_RULES", "qt.scenegraph.general=true;qt.rhi.general=true;qt.qpa.*=true;qt.quick3d.*=true", 1);   /* the last one is silent without Quick 3D */
	setenv("QZEPHYR_FRAME_LOG", "1", 1);   /* qzephyrwindow.cpp: a line per 60 frames + GL errors */
#endif
#ifdef CONFIG_QT_ENV
	/* CONFIG_QT_ENV="NAME=value;NAME2=value2": environment for the Qt
	 * application (there is no shell to set one).  Qt Quick / RHI knobs
	 * such as QSG_NO_DEPTH_BUFFER=1 or QSG_RENDER_LOOP=basic go here. */
	{
		static char env[sizeof(CONFIG_QT_ENV)];
		char *save = NULL;

		memcpy(env, CONFIG_QT_ENV, sizeof(env));
		for (char *kv = strtok_r(env, ";|", &save); kv; kv = strtok_r(NULL, ";|", &save)) {   /* '|' too: a ';' does not survive west's -DCONFIG_ path (CMake list) */
			char *eq = strchr(kv, '=');

			if (eq) {
				*eq = '\0';
				setenv(kv, eq + 1, 1);
			}
		}
	}
#endif
	return __real_main(1, argv);
}
