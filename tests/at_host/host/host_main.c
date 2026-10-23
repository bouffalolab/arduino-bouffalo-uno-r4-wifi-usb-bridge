/* api/Common.h 声明了无参的 main，C++ 编译单元里不能再定义带参数的 main，所以入口放在 C 文件里 */
int host_main(int argc, char **argv);

int main(int argc, char **argv)
{
    return host_main(argc, argv);
}
