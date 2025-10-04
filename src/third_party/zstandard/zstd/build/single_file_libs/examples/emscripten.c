/**
 * \file emscripten.c
 * Emscripten example of using the single-file \c zstddeclib. Draws a rotating
 * textured quad with data from the in-line Zstd compressed DXT1 texture (DXT1
 * being hardware compression, further compressed with Zstd).
 * \n
 * Compile using:
 * \code
 *	export CC_FLAGS="-Wall -Wextra -Werror -Os -g0 -flto --llvm-lto 3 -lGL -DNDEBUG=1"
 *	export EM_FLAGS="-s WASM=1 -s ENVIRONMENT=web --shell-file shell.html --closure 1"
 *	emcc $CC_FLAGS $EM_FLAGS -o out.html emscripten.c
 * \endcode
 * 
 * \author Carl Woffenden, Numfum GmbH (released under a CC0 license)
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "../zstddeclib.c"

//************************* Test Data (DXT texture) **************************/

/**
 * Zstd compressed DXT1 256x256 texture source.
 * \n
 * See \c testcard.png for the original.
 */
static uint8_t const srcZstd[] = {
#include "testcard-zstd.inl"
};

/**
 * Uncompressed size of \c #srcZstd.
 */
#define DXT1_256x256 32768

/**
 * Destination for decoding \c #srcZstd.
 */
static uint8_t dstDxt1[DXT1_256x256] = {};

#ifndef ZSTD_VERSION_MAJOR
/**
 * For the case where the decompression library hasn't been included we add a
 * dummy function to fake the process and stop the buffers being optimised out.
 */
size_t ZSTD_decompress(void* dst, size_t dstLen, const void* src, size_t srcLen) {
	return (memcmp(dst, src, (srcLen < dstLen) ? srcLen : dstLen)) ? dstLen : 0;
}
#endif

//*************************** Program and Shaders ***************************/

/**
 * Program object ID.
 */
static GLuint progId = 0;

/**
 * Vertex shader ID.
 */
static GLuint vertId = 0;

/**
 * Fragment shader ID.
 */
static GLuint fragId = 0;

//********************************* Uniforms *********************************/

/**
 * Quad rotation angle ID.
 */
static GLint uRotId = -1;

/**
 * Draw colour ID.
 */
static GLint uTx0Id = -1;

//******************************* Shader Source ******************************/

/**
 * Vertex shader to draw texture mapped polys with an applied rotation.
 */
static GLchar const vertShader2D[] =
#if GL_ES_VERSION_2_0
	"#version 100\n"
	"precision mediump float;\n"
#else
	"#version 120\n"
#endif
	"uniform   float uRot;"	// rotation
	"attribute vec2  aPos;"	// vertex position coords
	"attribute vec2  aUV0;"	// vertex texture UV0
	"varying   vec2  vUV0;"	// (passed to fragment shader)
	"void main() {"
	"	float cosA = cos(radians(uRot));"
	"	float sinA = sin(radians(uRot));"
	"	mat3 rot = mat3(cosA, -sinA, 0.0,"
	"					sinA,  cosA, 0.0,"
	"					0.0,   0.0,  1.0);"
	"	gl_Position = vec4(rot * vec3(aPos, 1.0), 1.0);"
	"	vUV0 = aUV0;"
	"}";

/**
 * Fragment shader for the above polys.
 */
static GLchar const fragShader2D[] =
#if GL_ES_VERSION_2_0
	"#version 100\n"
	"precision mediump float;\n"
#else
	"#version 120\n"
#endif
	"uniform sampler2D uTx0;"
	"varying vec2      vUV0;" // (passed from fragment shader)
	"void main() {"
	"	gl_FragColor = texture2D(uTx0, vUV0);"
	"}";

/**
 * Helper to compile a shader.
 * 
 * \param type shader type
 * \param text shader source
 * \return the shader ID (or zero if compilation failed)
 */
static GLuint compileShader(GLenum const type, const GLchar* text) {
	GLuint shader = glCreateShader(type);
	if (shader) {
		glShaderSource (shader, 1, &text, NULL);
		glCompileShader(shader);
		GLint compiled;
		glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
		if (compiled) {
			return shader;
		} else {
			GLint logLen;
			glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLen);
			if (logLen > 1) {
				GLchar*  logStr = malloc(logLen);
				glGetShaderInfoLog(shader, logLen, NULL, logStr);
			#ifndef NDEBUG
				printf("Shader compilation error: %s\n", logStr);
			#endif
				free(logStr);
			}
			glDeleteShader(shader);
		}
	}
	return 0;
}

//********************************** Helpers *********************************/

/**
 * Vertex position index.
 */
#define GL_VERT_POSXY_ID 0

/**
 * Vertex UV0 index.
 */
#define GL_VERT_TXUV0_ID 1

 /**
  * \c GL vec2 storage type.
  */
struct vec2 {
	float x;
	float y;
};

/**
 * Combined 2D vertex and 2D texture coordinates.
 */
struct posTex2d {
	struct vec2 pos;
	struct vec2 uv0;
};

//****************************************************************************/

/**
 * Current quad rotation angle (in degrees, updated per frame).
 */
static float rotDeg = 0.0f;

/**
 * Emscripten (single) GL context.
 */
static EMSCRIPTEN_WEBGL_CONTEXT_HANDLE glCtx = 0;

/**
 * Emscripten resize handler.
 */
static EM_BOOL resize(int type, const EmscriptenUiEvent* e, void* data) {
	double surfaceW;
	double surfaceH;
	if (emscripten_get_element_css_size   ("#canvas", &surfaceW, &surfaceH) == EMSCRIPTEN_RESULT_SUCCESS) {
		emscripten_set_canvas_element_size("#canvas",  surfaceW,  surfaceH);
		if (glCtx) {
			glViewport(0, 0, (int) surfaceW, (int) surfaceH);
		}
	}
	(void) type;
	(void) data;
	(void) e;
	return EM_FALSE;
}

/**
 * Boilerplate to create a WebGL context.
 */
static EM_BOOL initContext() {
	// Default attributes
	EmscriptenWebGLContextAttributes attr;
	emscripten_webgl_init_context_attributes(&attr);
	if ((glCtx = emscripten_webgl_create_context("#canvas", &attr))) {
		// Bind the context and fire a resize to get the initial size
		emscripten_webgl_make_context_current(glCtx);
		emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, NULL, EM_FALSE, resize);
		resize(0, NULL, NULL);
		return EM_TRUE;
	}
	return EM_FALSE;
}

/**
 * Called once per frame (clears the screen and draws the rotating quad).
 */
static void tick() {
	glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	if (uRotId >= 0) {
		glUniform1f(uRotId, rotDeg);
		rotDeg += 0.1f;
		if (rotDeg >= 360.0f) {
			rotDeg -= 360.0f;
		}
	}

	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
	glFlush();
}

/**
 * Creates the GL context, shaders and quad data, decompresses the Zstd data
 * and 'uploads' the resulting texture.
 * 
 * As a (naive) comparison, removing Zstd and building with "-Os -g0 s WASM=1
 * -lGL emscripten.c" results in a 15kB WebAssembly file; re-adding Zstd
 * increases the Wasm by 26kB.
 */
int main() {
	if (initContext()) {
		// Compile shaders and set the initial GL state
		if ((progId = glCreateProgram())) {
			 vertId = compileShader(GL_VERTEX_SHADER,   vertShader2D);
			 fragId = compileShader(GL_FRAGMENT_SHADER, fragShader2D);
			 
			 glBindAttribLocation(progId, GL_VERT_POSXY_ID, "aPos");
			 glBindAttribLocation(progId, GL_VERT_TXUV0_ID, "aUV0");
			 
			 glAttachShader(progId, vertId);
			 glAttachShader(progId, fragId);
			 glLinkProgram (progId);
			 glUseProgram  (progId);
			 uRotId = glGetUniformLocation(progId, "uRot");
			 uTx0Id = glGetUniformLocation(progId, "uTx0");
			 if (uTx0Id >= 0) {
				 glUniform1i(uTx0Id, 0);
			 }
			
			 glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			 glEnable(GL_BLEND);
			 glDisable(GL_DITHER);

			 glCullFace(GL_BACK);
			 glEnable(GL_CULL_FACE);
		}
		
		GLuint vertsBuf = 0;
		GLuint indexBuf = 0;
		GLuint txName   = 0;
		// Create the textured quad (vert positions then UVs)
		struct posTex2d verts2d[] = {
			{{-0.85f, -0.85f}, {0.0f, 0.0f}}, // BL
			{{ 0.85f, -0.85f}, {1.0f, 0.0f}}, // BR
			{{-0.85f,  0.85f}, {0.0f, 1.0f}}, // TL
			{{ 0.85f,  0.85f}, {1.0f, 1.0f}}, // TR
		};
		uint16_t index2d[] = {
			0, 1, 2,
			2, 1, 3,
		};
		glGenBuffers(1, &vertsBuf);
		glBindBuffer(GL_ARRAY_BUFFER, vertsBuf);
		glBufferData(GL_ARRAY_BUFFER,
			sizeof(verts2d), verts2d, GL_STATIC_DRAW);
		glVertexAttribPointer(GL_VERT_POSXY_ID, 2,
			GL_FLOAT, GL_FALSE, sizeof(struct posTex2d), 0);
		glVertexAttribPointer(GL_VERT_TXUV0_ID, 2,
			GL_FLOAT, GL_FALSE, sizeof(struct posTex2d),
				(void*) offsetof(struct posTex2d, uv0));
		glGenBuffers(1, &indexBuf);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuf);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER,
			sizeof(index2d), index2d, GL_STATIC_DRAW);
		glEnableVertexAttribArray(GL_VERT_POSXY_ID);
		glEnableVertexAttribArray(GL_VERT_TXUV0_ID);
		
		// Decode the Zstd data and create the texture
		if (ZSTD_decompress(dstDxt1, DXT1_256x256, srcZstd, sizeof srcZstd) == DXT1_256x256) {
			glGenTextures(1, &txName);
			glBindTexture(GL_TEXTURE_2D, txName);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glCompressedTexImage2D(GL_TEXTURE_2D, 0,
				GL_COMPRESSED_RGB_S3TC_DXT1_EXT,
					256, 256, 0, DXT1_256x256, dstDxt1);
		} else {
			printf("Failed to decode Zstd data\n");
		}
		emscripten_set_main_loop(tick, 0, EM_FALSE);
		emscripten_exit_with_live_runtime();
	}
	return EXIT_FAILURE;
}
