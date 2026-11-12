/* sohx_imconfig.h -- Ocarina-Of-Time-X: Dear ImGui's configuration (IMGUI_USER_CONFIG, tools/build.py) for the files that are
 * built against the real ImGui: SoH's own and ImGui's core. libultraship's are not: they see xbox/include/imgui.h's
 * types alone, and draw no GUI (LUS_NO_IMGUI). SoH calls ImGui all through its game code, but with no GUI nothing it
 * makes is ever drawn.
 *
 * The two options libultraship's patch sets in ImGui's imconfig.h (cmake/dependencies/patches/
 * imgui-fixes-and-config.patch), so SoH's code builds as it does on a PC. */
#pragma once

#define IMGUI_DEFINE_MATH_OPERATORS
#define ImTextureID void* /* the old way: https://github.com/ocornut/imgui/issues/1641 */

/* no file dialogs, clipboard or IME through the OS: there's no window to own them */
#define IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
#define IMGUI_DISABLE_WIN32_FUNCTIONS
#define IMGUI_DISABLE_FILE_FUNCTIONS /* (its .ini and log files: dummies) */

/* its asserts are for a GUI that's drawn: a stray call into it, with nothing drawn, isn't worth a stop */
#define IM_ASSERT(_EXPR) ((void)0)
