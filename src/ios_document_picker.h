#ifndef FFMPEG_GD_IOS_DOCUMENT_PICKER_H
#define FFMPEG_GD_IOS_DOCUMENT_PICKER_H

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

class IOSDocumentPicker : public RefCounted {
    GDCLASS(IOSDocumentPicker, RefCounted)

    void *delegate = nullptr;

protected:
    static void _bind_methods();

public:
    void open_media();
    void emit_file_selected(const String &p_path);
    void emit_canceled();

    IOSDocumentPicker();
    ~IOSDocumentPicker();
};

} // namespace godot

#endif // FFMPEG_GD_IOS_DOCUMENT_PICKER_H
