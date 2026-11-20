# ZaloSnooze

![ZaloSnooze](docs/assets/banner.vi.png)

[English](README.en.md) · [Bản tải xuống](https://github.com/chu0n/ZaloSnooze/releases) · [Báo lỗi](https://github.com/chu0n/ZaloSnooze/issues)

ZaloSnooze mở Zalo khi bạn đăng nhập Windows, đưa cửa sổ vào khay hệ thống hoặc thu nhỏ xuống thanh tác vụ, rồi tự thoát. Ứng dụng hỗ trợ Windows 10 và 11, không cần cài thêm runtime.

## Cài đặt

- **Setup:** Chạy tệp `Setup.exe` có tên ZaloSnooze trong trang tải xuống. Mặc định Setup chỉ cài cho tài khoản hiện tại.
- **Portable:** Giải nén toàn bộ gói `Portable-x86.zip` vào thư mục có quyền ghi, rồi mở `ZaloSnooze.exe`. Không chạy trực tiếp trong tệp ZIP.

Nếu chưa có tệp tải xuống, hãy xem [hướng dẫn tự build](docs/DEVELOPMENT.md). Các gói hiện chưa có chữ ký số.

## Sử dụng

1. Mở ZaloSnooze và chọn **Bật tự khởi động**. Nếu được hỏi, chọn có cho phép ứng dụng thay Zalo quản lý việc tự khởi động hay không.
2. Nếu muốn giữ nút Zalo trên thanh tác vụ, bật tùy chọn thu nhỏ bên dưới nút chính. Nếu không, ZaloSnooze sẽ đưa Zalo vào khay hệ thống.
3. Đóng cửa sổ cài đặt. Nếu Windows chưa báo có Internet khi đăng nhập, ZaloSnooze sẽ đợi trước khi mở Zalo.

Tắt tự khởi động trong ZaloSnooze để khôi phục các mục khởi động mà ứng dụng đã thay đổi, nếu chúng không xung đột với thay đổi mới hơn. Với bản cài cho mọi tài khoản, mỗi người cần làm bước này trước khi gỡ cài đặt.

**Trợ giúp → Kiểm tra cập nhật** mở trang phát hành trên GitHub. ZaloSnooze không tự kiểm tra, tải xuống hoặc cài đặt bản cập nhật trong nền.

## Giới hạn

Cửa sổ Zalo vẫn có thể lóe lên trước khi ZaloSnooze kịp xử lý. Ứng dụng không sửa tệp Zalo, không chèn mã và không chạy thường trực sau lần khởi động đầu tiên.

[Xử lý sự cố](docs/DEBUGGING.md) · [Kiến trúc](docs/ARCHITECTURE.md) · [Giấy phép MIT](LICENSE)
