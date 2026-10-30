/*
 * gltest: probe the Mali GPU through Android's EGL/GLES driver (libGLES_mali.so; Android 15's has no OpenCL any more)
 * and draw one verified frame off screen (a pbuffer: no SurfaceFlinger, no gralloc window).
 * Built for bionic without the NDK (see build.sh); runs inside the Android vendor chroot.
 *   gltest              probe, one frame drawn by a shader and read back
 *   gltest loop [SEC]   GPU stress: a heavy fragment shader over 512x512 for SEC seconds (0 = until killed),
 *                       printing frames and megapixels a second
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>
#include <stdint.h>

int printf(const char *, ...);
int atoi(const char *);
int strcmp(const char *, const char *);
int fflush(void *);
void *malloc(size_t);
struct timespec { long tv_sec; long tv_nsec; };
int clock_gettime(int, struct timespec *);

typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext;
typedef int32_t EGLint;
typedef unsigned int GLuint, GLenum;
typedef int GLint, GLsizei;
typedef float GLfloat;
#define EGL_DEFAULT_DISPLAY ((void *)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NONE 0x3038
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_VENDOR 0x3053
#define EGL_VERSION 0x3054
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_TRIANGLES 0x0004
#define GL_FLOAT 0x1406
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401

EGLDisplay eglGetDisplay(void *);
unsigned eglInitialize(EGLDisplay, EGLint *, EGLint *);
const char *eglQueryString(EGLDisplay, EGLint);
unsigned eglChooseConfig(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
EGLSurface eglCreatePbufferSurface(EGLDisplay, EGLConfig, const EGLint *);
EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
unsigned eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
EGLint eglGetError(void);
const char *glGetString(GLenum);
GLuint glCreateShader(GLenum);
void glShaderSource(GLuint, GLsizei, const char *const *, const GLint *);
void glCompileShader(GLuint);
void glGetShaderiv(GLuint, GLenum, GLint *);
void glGetShaderInfoLog(GLuint, GLsizei, GLsizei *, char *);
GLuint glCreateProgram(void);
void glAttachShader(GLuint, GLuint);
void glBindAttribLocation(GLuint, GLuint, const char *);
void glLinkProgram(GLuint);
void glGetProgramiv(GLuint, GLenum, GLint *);
void glUseProgram(GLuint);
GLint glGetUniformLocation(GLuint, const char *);
void glUniform1f(GLint, GLfloat);
void glVertexAttribPointer(GLuint, GLint, GLenum, unsigned char, GLsizei, const void *);
void glEnableVertexAttribArray(GLuint);
void glViewport(GLint, GLint, GLsizei, GLsizei);
void glDrawArrays(GLenum, GLint, GLsizei);
void glReadPixels(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
void glFinish(void);
GLenum glGetError(void);

static const char *vs =
	"attribute vec2 p; varying vec2 uv;"
	"void main() { uv = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }";
/* a gradient the CPU can check: r = x, g = y, b = 0.5 */
static const char *fs =
	"precision highp float; varying vec2 uv;"
	"void main() { gl_FragColor = vec4(uv.x, uv.y, 0.5, 1.0); }";
static const char *burn_fs =
	"precision highp float; varying vec2 uv; uniform float t;"
	"void main() { float x = uv.x + t, y = uv.y;"
	"  for (int k = 0; k < 64; k++) { x = sqrt(x * x + 1.0) * 0.5 + sin(x) * cos(y); y = fract(y * 1.7 + x); }"
	"  gl_FragColor = vec4(fract(x), y, 0.5, 1.0); }";
/* one triangle over the whole viewport */
static const GLfloat tri[] = { -1, -1, 3, -1, -1, 3 };

static double now(void)
{
	struct timespec ts;

	clock_gettime(1, &ts);	/* CLOCK_MONOTONIC */
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static GLuint shader(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	GLint ok = 0;
	char log[512];

	glShaderSource(s, 1, &src, 0);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		log[0] = 0;
		glGetShaderInfoLog(s, sizeof(log), 0, log);
		printf("FAIL: shader: %s\n", log);
		return 0;
	}
	return s;
}

static GLuint program(const char *fsrc)
{
	GLuint v = shader(GL_VERTEX_SHADER, vs), f = shader(GL_FRAGMENT_SHADER, fsrc), p;
	GLint ok = 0;

	if (!v || !f)
		return 0;
	p = glCreateProgram();
	glAttachShader(p, v);
	glAttachShader(p, f);
	glBindAttribLocation(p, 0, "p");
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		printf("FAIL: link\n");
		return 0;
	}
	glUseProgram(p);
	glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, tri);
	glEnableVertexAttribArray(0);
	return p;
}

static int setup(int w, int h)
{
	static const EGLint cattr[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
					EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
					EGL_NONE };
	static const EGLint xattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLint sattr[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
	EGLint major = 0, minor = 0, n = 0;
	EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	EGLConfig c;
	EGLSurface s;
	EGLContext x;

	if (!d || !eglInitialize(d, &major, &minor)) {
		printf("FAIL: eglInitialize (0x%x)\n", eglGetError());
		return -1;
	}
	printf("EGL %d.%d: %s, %s\n", major, minor, eglQueryString(d, EGL_VENDOR), eglQueryString(d, EGL_VERSION));
	if (!eglChooseConfig(d, cattr, &c, 1, &n) || n < 1) {
		printf("FAIL: no RGBA8888 pbuffer config (0x%x)\n", eglGetError());
		return -1;
	}
	s = eglCreatePbufferSurface(d, c, sattr);
	x = eglCreateContext(d, c, EGL_NO_CONTEXT, xattr);
	if (!s || !x || !eglMakeCurrent(d, s, s, x)) {
		printf("FAIL: pbuffer/context (0x%x)\n", eglGetError());
		return -1;
	}
	printf("GL: %s | %s | %s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));
	glViewport(0, 0, w, h);
	return 0;
}

static int burn(int sec)
{
	const int w = 512, h = 512;
	unsigned char px[4];
	GLuint prog;
	GLint t;
	double t0, last;
	long frames = 0, lf = 0;

	if (setup(w, h) || !(prog = program(burn_fs)))
		return 1;
	t = glGetUniformLocation(prog, "t");
	t0 = last = now();
	for (;;) {
		glUniform1f(t, (float)frames * 0.01f);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);	/* waits for the frame */
		frames++;
		if (now() - last >= 1.0) {
			double dt = now() - last;

			printf("%.0f s: %.1f frames/s, %.0f Mpx/s\n", now() - t0, (frames - lf) / dt,
			       (frames - lf) * (double)w * h / dt / 1e6);
			fflush(0);
			lf = frames;
			last = now();
		}
		if (sec > 0 && now() - t0 >= sec)
			break;
	}
	printf("PASS: %ld frames in %.0f s\n", frames, now() - t0);
	return glGetError() ? 1 : 0;
}

int main(int argc, char **argv, char **envp)
{
	const int w = 64, h = 64;
	unsigned char *px;
	int bad = 0, i, j;
	double t0;

	(void)envp;
	if (argc > 1 && !strcmp(argv[1], "loop"))
		return burn(argc > 2 ? atoi(argv[2]) : 10);
	if (setup(w, h) || !program(fs))
		return 1;
	px = malloc(w * h * 4);
	t0 = now();
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++) {
			unsigned char *p = px + (j * w + i) * 4;
			int r = (i * 2 + 1) * 255 / (2 * w), g = (j * 2 + 1) * 255 / (2 * h);

			if (p[0] - r > 2 || r - p[0] > 2 || p[1] - g > 2 || g - p[1] > 2 || p[2] < 126 || p[2] > 129 ||
			    p[3] != 255)
				bad++;
		}
	if (glGetError() || bad) {
		printf("FAIL: %d of %d pixels wrong\n", bad, w * h);
		return 1;
	}
	printf("PASS: %dx%d frame drawn by the GPU and checked (%.1f ms)\n", w, h, (now() - t0) * 1e3);
	return 0;
}
