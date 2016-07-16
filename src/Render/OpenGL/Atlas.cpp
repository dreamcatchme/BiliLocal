#include "Common.h"
#include "Atlas.h"
#include "OpenGLRenderPrivate.h"
#include "../../Graphic/GraphicPrivate.h"

const int Atlas::MaxSize = 2048;
const int Atlas::Padding = 2;
const int Atlas::Spacing = 0;

Atlas::Atlas(OpenGLRenderPrivate *render)
	: render(render)
	, refNum(0)
	, cached(0)
{
	render->glGenTextures(1, &cached);
	render->glBindTexture(GL_TEXTURE_2D, cached);
	GLenum format = render->pixelFormat(2, true);
	render->glTexImage2D(
		GL_TEXTURE_2D,
		0,
		format,
		MaxSize, MaxSize,
		0,
		format,
		GL_UNSIGNED_BYTE,
		nullptr);
	render->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	render->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	render->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	render->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

Atlas::~Atlas()
{
	render->glDeleteTextures(1, &cached);
}

void Atlas::insert(QList<QImage> &source, int effect, QList<Sprite> &result)
{
	for (auto iter = source.begin(); iter != source.end();) {
		bool fitted = false;
		auto offset = QPoint(0, 0);
		for (QSize &size : usages) {
			if (size.height() >= iter->height() && size.width() + iter->width() <= MaxSize) {
				offset.rx() = size.width();
				size.rwidth() += iter->width();
				fitted = true;
				break;
			}
			offset.ry() += size.height();
		}
		if (!fitted && offset.y() + iter->height() <= MaxSize) {
			usages.append(iter->size());
			fitted = true;
		}
		if (fitted) {
			QRect data = QRect(offset, iter->size());
			QOpenGLShaderProgram *p = nullptr;
			switch (effect) {
			case 0:
				p = &render->resource->program[OpenGLRenderPrivate::Stro];
				break;
			case 1:
				p = &render->resource->program[OpenGLRenderPrivate::Proj];
				break;
			case 2:
				p = &render->resource->program[OpenGLRenderPrivate::Glow];
				break;
			default:
				return;
			}
			const int spc = Atlas::Spacing;
			QRectF draw(data.adjusted(spc, spc, -spc, -spc));

			render->appendLoadCall(draw, p, data, iter->constBits());

			Sprite sprite(this);
			sprite.rect = draw;
			result.append(sprite);
			iter = source.erase(iter);
		}
		else {
			return;
		}
	}
}

AtlasMgr::AtlasMgr(OpenGLRenderPrivate *render)
	: render(render)
{
	render->glGenTextures(1, &upload);
	render->glBindTexture(GL_TEXTURE_2D, upload);
	render->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	render->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	render->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	render->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	GLenum format = render->pixelFormat(1);
	render->glTexImage2D(
		GL_TEXTURE_2D,
		0,
		format,
		Atlas::MaxSize,
		Atlas::MaxSize,
		0,
		format,
		GL_UNSIGNED_BYTE,
		nullptr);
	render->glGenFramebuffers(1, &buffer);
}

AtlasMgr::~AtlasMgr()
{
	render->glDeleteFramebuffers(1, &buffer);
	render->glDeleteTextures(1, &upload);
}

uint qHash(const AtlasMgr::CreateInfo &i, uint seed = 0)
{
	uint h = ::qHash(i.effect, seed);
	h = (h << 1) ^ ::qHash(i.text, seed);
	h = (h << 1) ^ ::qHash(i.font, seed);
	return h;
}

bool operator ==(const AtlasMgr::CreateInfo &f, const AtlasMgr::CreateInfo &s)
{
	return f.text == s.text && f.font == s.font && f.effect == s.effect;
}

void AtlasMgr::insert(CreateInfo info, QList<Sprite> &result, QSize &size)
{
	auto itr = cached.find(info);
	if (cached.end() != itr) {
		result = itr->first; size = itr->second;
		return;
	}

	const int exp = Atlas::Padding + Atlas::Spacing;
	QImage source(GraphicPrivate::getSize(info.text, info.font) + QSize(exp, exp) * 2, QImage::Format_Alpha8);
	source.fill(Qt::transparent);
	QPainter painter(&source);
	painter.setFont(info.font);
	painter.setCompositionMode(QPainter::CompositionMode_Source);
	painter.drawText(source.rect().adjusted(exp, exp, -exp, -exp), info.text);
	painter.end();

	QList<QImage> splited;
	size = QSize(0, 0);
	int sw = source.width(), sh = source.height();
	for (int y = 0; y < sh; y += Atlas::MaxSize) {
		++size.rheight();
		for (int x = 0; x < sw; x += Atlas::MaxSize) {
			int w = std::min<int>(sw - x, Atlas::MaxSize);
			int h = std::min<int>(sh - y, Atlas::MaxSize);
			if (w == sw && h == sh) {
				splited.append(source);
			}
			else {
				splited.append(source.copy(x, y, w, h));
			}
		}
	}
	size.rwidth() = size.height() == 0 ? 0 : splited.size() / size.height();

	result.clear();
	for (;;) {
		if (atlases.isEmpty() == false) {
			atlases.front()->insert(splited, info.effect, result);
		}
		if (splited.isEmpty() == false) {
			shuffle();
		}
		else {
			break;
		}
	}

	if (!result.isEmpty()) {
		cached.insert(info, qMakePair(result, size));
	}
}

void AtlasMgr::shuffle()
{
	if (atlases.isEmpty()) {
		atlases.prepend(new Atlas(render));
	}
	else {
		render->flushLoad();

		for (auto iter = atlases.begin() + 1; iter != atlases.end(); ++iter) {
			if ((*iter)->isEmpty()) {
				Atlas *at = *iter;
				atlases.erase(iter);
				atlases.prepend(at);
				break;
			}
		}
		if (atlases.front()->isEmpty() == false) {
			atlases.prepend(new Atlas(render));
		}
	}

	GLuint target = atlases.front()->getTexture();
	GLuint window = QOpenGLContext::currentContext()->defaultFramebufferObject();
	render->glBindFramebuffer(GL_FRAMEBUFFER, buffer);
	if (render->extensions.contains("clear_texture")) {
		static uchar bits[4];
		GLenum format = render->pixelFormat(1);

		typedef void (QOPENGLF_APIENTRYP ClearTexImageFunc) (
			GLuint texture,
			int level,
			GLenum format,
			GLenum type,
			const void *data);

		static ClearTexImageFunc glClearTexImage = (ClearTexImageFunc)QOpenGLContext::currentContext()->getProcAddress("glClearTexImage");
		glClearTexImage(
			upload,
			0,
			format,
			GL_UNSIGNED_BYTE,
			bits);
	}
	else if (render->extensions.contains("texture_rg")) {
		render->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, upload, 0);
		render->glViewport(0, 0, Atlas::MaxSize, Atlas::MaxSize);
		render->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
		render->glClear(GL_COLOR_BUFFER_BIT);
	}
	else{
		static uchar bits[Atlas::MaxSize * Atlas::MaxSize];
		GLenum format = render->pixelFormat(1);
		render->glBindTexture(GL_TEXTURE_2D, upload);
		render->glTexImage2D(
			GL_TEXTURE_2D,
			0,
			format,
			Atlas::MaxSize,
			Atlas::MaxSize,
			0,
			format,
			GL_UNSIGNED_BYTE,
			bits);
	}
	render->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
	render->glBindFramebuffer(GL_FRAMEBUFFER, window);
	cached.clear();
}

void AtlasMgr::squeeze(int timeout)
{
	for (auto iter = atlases.begin(); iter != atlases.end() && atlases.size() > 2;) {
		if ((*iter)->expired(timeout)) {
			delete *iter;
			iter = atlases.erase(iter);
		}
		else {
			++iter;
		}
	}
}
