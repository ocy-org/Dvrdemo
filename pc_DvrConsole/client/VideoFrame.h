#pragma once
#include <QByteArray>
#include <QMetaType>
#include <memory>

// 跨线程视频帧：YUV420三平面，值语义（shared_ptr携带）
struct VideoFrame {
    int w = 0, h = 0;
    QByteArray y, u, v;
    int yStride = 0, uStride = 0, vStride = 0;
};

// ★让 std::shared_ptr<VideoFrame> 成为Qt认识的元类型（跨线程排队投递的前提）
Q_DECLARE_METATYPE(std::shared_ptr<VideoFrame>)
