/* gl_absent.c - the OpenGL 1.1 entry points nvn_gl.c's old OpenGL back end still names.
 *
 * Linux has Vulkan as its only renderer: nvn_gl.c keeps the front end (command lists, the render thread) and its
 * OpenGL back end is no longer reachable (gfx_options.c accepts no renderer but vulkan). The back end's code is still
 * compiled, so these definitions let the program link without libGL; each one stops the program with a message if it
 * is ever called. To go when the OpenGL back end is taken out of nvn_gl.c (PORT_TO_LINUX.md). */
#include <stdio.h>
#include <stdlib.h>
#define GL_GLEXT_LEGACY 1
#include <GL/gl.h>

static void absent(const char *name) {
    fprintf(stderr, "gl_absent: %s called: the OpenGL back end is not part of the Linux build\n", name);
    fflush(stdout);
    abort();
}

void GLAPIENTRY glAlphaFunc(GLenum func, GLclampf ref) { (void)func; (void)ref;  absent("glAlphaFunc"); }
void GLAPIENTRY glBindTexture(GLenum target, GLuint texture) { (void)target; (void)texture;  absent("glBindTexture"); }
void GLAPIENTRY glClear(GLbitfield mask) { (void)mask;  absent("glClear"); }
void GLAPIENTRY glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) { (void)red; (void)green; (void)blue; (void)alpha;  absent("glClearColor"); }
void GLAPIENTRY glClearDepth(GLclampd depth) { (void)depth;  absent("glClearDepth"); }
void GLAPIENTRY glClearStencil(GLint s) { (void)s;  absent("glClearStencil"); }
void GLAPIENTRY glCullFace(GLenum mode) { (void)mode;  absent("glCullFace"); }
void GLAPIENTRY glDeleteTextures(GLsizei n, const GLuint *textures) { (void)n; (void)textures;  absent("glDeleteTextures"); }
void GLAPIENTRY glDepthFunc(GLenum func) { (void)func;  absent("glDepthFunc"); }
void GLAPIENTRY glDepthMask(GLboolean flag) { (void)flag;  absent("glDepthMask"); }
void GLAPIENTRY glDisable(GLenum cap) { (void)cap;  absent("glDisable"); }
void GLAPIENTRY glDrawArrays(GLenum mode, GLint first, GLsizei count) { (void)mode; (void)first; (void)count;  absent("glDrawArrays"); }
void GLAPIENTRY glEnable(GLenum cap) { (void)cap;  absent("glEnable"); }
void GLAPIENTRY glFinish(void) {  absent("glFinish"); }
void GLAPIENTRY glFlush(void) {  absent("glFlush"); }
void GLAPIENTRY glFrontFace(GLenum mode) { (void)mode;  absent("glFrontFace"); }
void GLAPIENTRY glGenTextures(GLsizei n, GLuint *textures) { (void)n; (void)textures;  absent("glGenTextures"); }
void GLAPIENTRY glGetBooleanv(GLenum pname, GLboolean *params) { (void)pname; (void)params;  absent("glGetBooleanv"); }
GLenum GLAPIENTRY glGetError(void) {  absent("glGetError"); return 0; }
void GLAPIENTRY glGetFloatv(GLenum pname, GLfloat *params) { (void)pname; (void)params;  absent("glGetFloatv"); }
void GLAPIENTRY glGetIntegerv(GLenum pname, GLint *params) { (void)pname; (void)params;  absent("glGetIntegerv"); }
const GLubyte * GLAPIENTRY glGetString(GLenum name) { (void)name;  absent("glGetString"); return 0; }
void GLAPIENTRY glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid *pixels) { (void)target; (void)level; (void)format; (void)type; (void)pixels;  absent("glGetTexImage"); }
void GLAPIENTRY glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params) { (void)target; (void)level; (void)pname; (void)params;  absent("glGetTexLevelParameteriv"); }
GLboolean GLAPIENTRY glIsEnabled(GLenum cap) { (void)cap;  absent("glIsEnabled"); return 0; }
void GLAPIENTRY glLineWidth(GLfloat width) { (void)width;  absent("glLineWidth"); }
void GLAPIENTRY glLogicOp(GLenum opcode) { (void)opcode;  absent("glLogicOp"); }
void GLAPIENTRY glPixelStorei(GLenum pname, GLint param) { (void)pname; (void)param;  absent("glPixelStorei"); }
void GLAPIENTRY glPointSize(GLfloat size) { (void)size;  absent("glPointSize"); }
void GLAPIENTRY glPolygonMode(GLenum face, GLenum mode) { (void)face; (void)mode;  absent("glPolygonMode"); }
void GLAPIENTRY glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels) { (void)x; (void)y; (void)width; (void)height; (void)format; (void)type; (void)pixels;  absent("glReadPixels"); }
void GLAPIENTRY glScissor(GLint x, GLint y, GLsizei width, GLsizei height) { (void)x; (void)y; (void)width; (void)height;  absent("glScissor"); }
void GLAPIENTRY glStencilFunc(GLenum func, GLint ref, GLuint mask) { (void)func; (void)ref; (void)mask;  absent("glStencilFunc"); }
void GLAPIENTRY glTexParameteri(GLenum target, GLenum pname, GLint param) { (void)target; (void)pname; (void)param;  absent("glTexParameteri"); }
void GLAPIENTRY glTexParameteriv(GLenum target, GLenum pname, const GLint *params) { (void)target; (void)pname; (void)params;  absent("glTexParameteriv"); }
void GLAPIENTRY glTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type, const GLvoid *pixels) { (void)target; (void)level; (void)xoffset; (void)width; (void)format; (void)type; (void)pixels;  absent("glTexSubImage1D"); }
void GLAPIENTRY glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels) { (void)target; (void)level; (void)xoffset; (void)yoffset; (void)width; (void)height; (void)format; (void)type; (void)pixels;  absent("glTexSubImage2D"); }
void GLAPIENTRY glViewport(GLint x, GLint y, GLsizei width, GLsizei height) { (void)x; (void)y; (void)width; (void)height;  absent("glViewport"); }
