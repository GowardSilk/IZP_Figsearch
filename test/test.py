import sys
import subprocess
from dataclasses import dataclass
import random
from time import time
import os

DEF_BMP_SIZE = (1920, 1080)
N_TESTS: int = 100


@dataclass
class BitmapSize:
    def __init__(self, height = DEF_BMP_SIZE[1], width = DEF_BMP_SIZE[0]):
        self.width = width
        self.height = height

    width: int
    height: int


def generate_pix() -> str:
    return "1" if random.random() > 0.5 else "0"


def chance() -> bool:
    return random.random() > 0.5


def cmd_test(cmd_type: str, exec: str) -> None:
    def _generate_bmp(size: BitmapSize, loc: str, generate_valid: bool = True):
        with open(loc, "w+") as file:
            if generate_valid:
                file.write(str(size.height) + " " + str(size.width) + "\n")
                file.writelines(
                    " ".join(generate_pix() for _ in range(size.width)) + "\n"
                    for _ in range(size.height)
                )
            else:
                file.write(
                    str(random.randint(0, size.height) if chance() else size.height)
                    + " "
                    + str(random.randint(0, size.width) if chance() else size.width)
                    + "\n"
                )
                file.writelines(
                    " ".join(generate_pix() for _ in range(size.width)) + "\n"
                    for _ in range(size.height)
                )

    def _run_unit_time(exec: str) -> float:
        bmp: str = f"pics/bmp_{random.randint(0, 10000)}"
        _generate_bmp(BitmapSize(), bmp)
        print([exec, "test", bmp])
        begin = time()
        ret = subprocess.run([exec, "test", bmp])
        end = time()
        if ret.returncode != 0:
            raise Exception(
                f"Test \x1b[31mfailed\x1b[0m! {exec} returned: {ret.returncode}; expected: 0"
            )
        print(f"Test took: {end - begin}ms")
        return end - begin

    def _run_unit(exec: str, should_fail: bool) -> None:
        bmp: str = f"pics/bmp_{random.randint(0, 10000)}"
        _generate_bmp(BitmapSize(), bmp, should_fail)
        print([exec, "test", bmp])
        ret = subprocess.run([exec, "test", bmp])
        if ret.returncode != 0:
            print(
                f"Test \x1b[31mfailed\x1b[0m! {exec} returned: {ret.returncode}; expected: 0"
            )
            input("Press enter to continue...")
        else:
            print(f"Test \x1b[33mpassed\x1b[0m!")

    match (cmd_type):
        case "time":
            print("[timed test]\n")
            average: float = 0
            for _ in range(N_TESTS):
                average += _run_unit(exec)
            average /= n_tests
            print(f"Average: {average}")

        case "functionality":
            for _ in range(N_TESTS):
                _run_unit(exec, chance())

        case _:
            assert False

    return None


@dataclass
class Point:
    x: int
    y: int


@dataclass
class Line:
    begin: Point
    end: Point

    def hlength(self) -> int:
        return self.end.y - self.begin.y

    def vlength(self) -> int:
        return self.end.x - self.begin.x

    def __str__(self) -> str:
        return f"{self.begin.y} {self.begin.x} {self.end.y} {self.end.x}"


def cmd_hline(cmd_type: str, exec: str) -> None:
    def _generate_hlines(row_offset: int, max_col: int) -> tuple[Line, list[Line]]:
        hlines: list[Line] = []
        max_length: int = random.randint(0, max_col // 3)
        last_pos: int = 0
        max_line: Line = Line(Point(0, 0), Point(0, 0))

        while last_pos + max_length <= max_col:
            begin = Point(row_offset, random.randint(last_pos, last_pos + max_length))
            last_pos = begin.y
            end: Point

            if last_pos == max_col:
                end = Point(row_offset, random.randint(last_pos, last_pos))
            else:
                if last_pos + max_length > max_col:
                    end = Point(row_offset, random.randint(last_pos, max_col))
                else:
                    end = Point(row_offset, random.randint(last_pos, last_pos + max_length))

            last_pos = end.y + 1
            hlines.append(Line(begin, end))

            if (hlines[-1].hlength() > max_line.hlength()):
                max_line = hlines[-1]

        return (max_line, hlines)

    def _generate_bmp(size: BitmapSize, loc: str) -> Line:
        with open(loc, "w+") as file:
            file.write(str(size.height) + " " + str(size.width) + "\n")
            # generate hlines in each row
            max_line: Line = Line(Point(0, 0), Point(0, 0))
            for row in range(size.height):
                # generate hlines in row
                max_row_line, hlines = _generate_hlines(row, size.width)
                if max_row_line.hlength() > max_line.hlength():
                    max_line = max_row_line
                hlines_str: list[chr] = ["0" for _ in range(size.width)]
                for _ in range(len(hlines)):
                    for j in range(hlines[-1].hlength()):
                        hlines_str[j + hlines[-1].begin.y] = "1"
                    hlines.pop()

                file.write(' '.join(hlines_str) + "\n")
            
            return max_line

    def _run_unit_time(exec: str) -> float:
        bmp: str = f"pics/bmp_{random.randint(0, 10000)}"
        _generate_bmp(BitmapSize(), bmp)
        print([exec, "test", bmp])
        begin = time()
        ret = subprocess.run([exec, "test", bmp])
        end = time()
        if ret.returncode != 0:
            raise Exception(
                f"Test \x1b[31mfailed\x1b[0m! {exec} returned: {ret.returncode}; expected: 0"
            )
        print(f"Test took: {end - begin}ms")
        return end - begin

    def _run_unit(exec: str) -> None:
        bmp: str = f"pics/bmp_{random.randint(0, 10000)}"
        max_line: Line = _generate_bmp(BitmapSize(), bmp)
        print([exec, "hline", bmp])
        ret = subprocess.run([exec, "test", bmp])
        if ret.returncode != 0:
            print(
                f"Test \x1b[31mfailed\x1b[0m! {exec} returned: {ret.returncode}; expected: 0"
            )
            input("Press enter to continue...")
        elif ret.stdout != str(max_line):
            print(f"Test \x1b[31mfailed\x1b[0m! Expected: {str(max_line)}; but received: {ret.stdout}")
            input("Press enter to continue...")
        else:
            print(f"Test \x1b[33mpassed\x1b[0m!")

    match cmd_type:
        case "time":
            print("[timed test]\n")
            assert False  # todo
        case "functionality":
            for _ in range(N_TESTS):
                _run_unit(exec)
        case _:
            assert False

    return None


def cmd_vline(exec: str) -> None:
    return None


def cmd_square(exec: str) -> None:
    assert False  # todo
    return None


def prepare() -> None:
    if os.path.exists("pics"):
        for filename in os.listdir("pics"):
            file_path = os.path.join("pics", filename)
            assert os.path.isfile(file_path)
            os.remove(file_path)
    else:
        os.makedirs("pics", exist_ok=True)

    return None


if __name__ == "__main__":
    assert len(sys.argv) > 1

    if len(sys.argv) == 3:
        prepare()
        # cmd_test(sys.argv[1], sys.argv[2])
        cmd_hline(sys.argv[1], sys.argv[2])
    elif len(sys.argv) == 2:
        prepare()
        # cmd_test("functionality", sys.argv[1])
        cmd_hline("functionality", sys.argv[1])
    else:
        raise Exception("Invalid number of arguments!")
