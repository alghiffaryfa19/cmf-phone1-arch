// Offscreen GLES2 probe for libhybris: pbuffer, clear + shaded triangle, read back.
// Proves the Mali driver actually renders without touching the display.
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <time.h>

static const char *vs = "attribute vec2 p; void main(){ gl_Position = vec4(p,0.0,1.0); }";
static const char *fs = "precision mediump float; void main(){ gl_FragColor = vec4(0.0,1.0,0.0,1.0); }";

static GLuint sh(GLenum t, const char *s)
{
	GLuint h = glCreateShader(t);
	glShaderSource(h, 1, &s, NULL);
	glCompileShader(h);
	return h;
}

int main(void)
{
	EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	EGLint maj, min, n;
	EGLConfig c;
	const EGLint ca[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
			      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
	const EGLint sa[] = { EGL_WIDTH, 256, EGL_HEIGHT, 256, EGL_NONE };
	const EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	const GLfloat tri[] = { -1, -1, 1, -1, -1, 1 };
	unsigned char px[2][4];
	struct timespec t0, t1;

	if (!eglInitialize(d, &maj, &min) || !eglChooseConfig(d, ca, &c, 1, &n) || n < 1) {
		fprintf(stderr, "EGL init/config failed: 0x%x\n", eglGetError());
		return 1;
	}
	EGLSurface s = eglCreatePbufferSurface(d, c, sa);
	EGLContext x = eglCreateContext(d, c, EGL_NO_CONTEXT, xa);
	if (s == EGL_NO_SURFACE || x == EGL_NO_CONTEXT || !eglMakeCurrent(d, s, s, x)) {
		fprintf(stderr, "surface/context failed: 0x%x\n", eglGetError());
		return 1;
	}
	printf("GL_VENDOR   %s\nGL_RENDERER %s\nGL_VERSION  %s\n",
	       glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));

	GLuint pr = glCreateProgram();
	glAttachShader(pr, sh(GL_VERTEX_SHADER, vs));
	glAttachShader(pr, sh(GL_FRAGMENT_SHADER, fs));
	glBindAttribLocation(pr, 0, "p");
	glLinkProgram(pr);
	glUseProgram(pr);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
	glEnableVertexAttribArray(0);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < 1000; i++) {
		glClearColor(1, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLES, 0, 3);
	}
	glFinish();
	clock_gettime(CLOCK_MONOTONIC, &t1);

	glReadPixels(10, 10, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px[0]);    /* inside triangle */
	glReadPixels(245, 245, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px[1]);  /* outside */
	printf("pixel in triangle  %3d %3d %3d %3d (expect green)\n", px[0][0], px[0][1], px[0][2], px[0][3]);
	printf("pixel outside      %3d %3d %3d %3d (expect red)\n", px[1][0], px[1][1], px[1][2], px[1][3]);
	printf("1000 frames in %.1f ms, glGetError 0x%x\n",
	       (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6, glGetError());
	return !(px[0][1] > 200 && px[0][0] < 50 && px[1][0] > 200 && px[1][1] < 50);
}
