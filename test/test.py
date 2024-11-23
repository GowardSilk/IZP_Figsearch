import sys
import subprocess
from dataclasses import dataclass
import random
from time import time
import os

DEF_BMP_SIZE = (1920, 1080)
N_TESTS: int = 100


def curr_dir() -> str:
    return os.path.dirname(os.path.abspath(__file__))


class Command:
    class Command_Randomized:
        def __init__(self, test: bool, space: bool):
            self.test = test
            self.space = space

    def __init__(self, cmd: list[str]):
        self.randomized = Command.Command_Randomized(
            test="--randomized-test" in cmd, space="--randomized-whitespace" in cmd
        )
        self.verbose = "--verbose" in cmd  # TODO
        if len(cmd) > 1:
            assert cmd[0] in {self.__FUNCTIONALITY, self.__TIME}
            self.cmd_type = cmd[0]
            assert os.path.exists(cmd[1])
            self.exec = cmd[1]
        elif len(cmd) == 1:
            self.cmd_type = __FUNCTIONALITY
            assert os.path.exists(cmd[1])
            self.exec = cmd[0]
        else:
            raise Exception("Invalid number of arguments!")

    def is_of_functional(self) -> bool:
        return self.cmd_type == self.__FUNCTIONALITY

    def is_of_time(self) -> bool:
        return self.cmd_type == self.__TIME

    @property
    def is_random_space(self) -> bool:
        return self.randomized.space

    @property
    def is_random_test(self) -> bool:
        return self.randomized.test

    @property
    def is_verbose(self) -> bool:
        return self.verbose

    @staticmethod
    def on_help(self) -> None:
        assert False  # TODO

    __FUNCTIONALITY = "functionality"
    __TIME = "time"


@dataclass
class BitmapSize:
    def __init__(self, height=DEF_BMP_SIZE[1], width=DEF_BMP_SIZE[0]):
        self.width = width
        self.height = height

    width: int
    height: int


def generate_pix() -> str:
    return "1" if random.random() > 0.5 else "0"


def chance() -> bool:
    return random.random() > 0.5


def print_unit_test_fmt(exec_args: list[str]) -> None:
    print("=============== Test ===============")
    print(f"running figsearch: {exec_args[0]}")
    print(f"command: {exec_args[1]}")
    print(f"bitmap location: {exec_args[2]}")
    print(f"--------")


def subprocess_evaluate_timed(run_exec: list[str], **kwargs) -> tuple[bool, float]:
    print_unit_test_fmt(run_exec)
    begin = time()
    ret = subprocess.run(run_exec, capture_output=True, text=True, **kwargs)
    end = time()
    delta = end - begin
    if ret.returncode != 0:
        print(
            f"Test \x1b[31mfailed\x1b[0m! {run_exec[0]} returned: {ret.returncode}; expected: 0"
        )
        input("Press any key to continue...")
        return (False, delta)

    return (True, delta)


def subprocess_evaluate(run_exec: list[str], expected_output: str, **kwargs) -> bool:
    print_unit_test_fmt(run_exec)
    ret = subprocess.run(
        run_exec,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        **kwargs,
    )

    actual_output = ""
    if ret.returncode == 0:
        actual_output = ret.stdout.strip()
    else:
        actual_output = ret.stderr.strip()
    # exact match, test passed
    if actual_output == expected_output:
        print(f"Test \x1b[33mpassed\x1b[0m!")
        return True
    # partial match
    elif expected_output in actual_output:
        print(
            f"Test \x1b[31muncertain\x1b[0m! Expected output is contained within the program's output.\n"
            f"Expected: {expected_output}\n"
            f"Actual: {actual_output}\n"
        )
        user_decision = input("Did the test pass? (y/n): ").strip().lower()
        if user_decision == "y":
            print("Test marked as \x1b[33mpassed\x1b[0m based on user input.")
            input("Press any key to continue...")
            return True
        else:
            print("Test marked as \x1b[31mfailed\x1b[0m based on user input.")
            input("Press any key to continue...")
            return False
    # no match, test failed
    else:
        print(
            f"Test \x1b[31mfailed\x1b[0m! Expected: {expected_output}; but received: {actual_output}"
        )
        input("Press any key to continue...")
        return False


def random_space(gen_random: bool, default: str = " ") -> str:
    return (
        default
        if not gen_random
        else "".join(
            [
                random.choice([" ", "\t", "\n", "\v", "\f", "\r"])
                for i in range(random.randint(1, 10))
            ]
        )
    )


def cmd_test(cmd: Command) -> None:
    def _generate_bmp(
        size: BitmapSize, loc: str, gen_valid: bool, is_random_space: bool
    ):
        with open(loc, "w+") as file:
            if gen_valid:
                file.write(
                    str(size.height)
                    + random_space(is_random_space)
                    + str(size.width)
                    + random_space(is_random_space, "\n")
                )
                file.writelines(
                    random_space(is_random_space).join(
                        generate_pix() for _ in range(size.width)
                    )
                    + random_space(is_random_space, "\n")
                    for _ in range(size.height)
                )
            else:
                file.write(
                    str(random.randint(0, size.height) if chance() else size.height)
                    + random_space(is_random_space)
                    + str(random.randint(0, size.width) if chance() else size.width)
                    + random_space(is_random_space, "\n")
                )
                file.writelines(
                    random_space(is_random_space).join(
                        generate_pix() if chance() else "x" for _ in range(size.width)
                    )
                    + random_space(is_random_space, "\n")
                    for _ in range(size.height)
                )

    def _run_unit_time(exec: str, gen_random_space: bool) -> float:
        bmp: str = f"{curr_dir()}/pics/bmp_{random.randint(0, 10000)}"
        _generate_bmp(BitmapSize(), bmp, True, gen_random_space)
        passed, delta = subprocess_evaluate_timed([exec, "test", bmp])
        assert passed
        print(f"Test took: {delta}s.")
        return delta

    def _run_unit(exec: str, gen_valid: bool, gen_random_space: bool) -> None:
        bmp: str = f"{curr_dir()}/pics/bmp_{random.randint(0, 10000)}"
        _generate_bmp(BitmapSize(), bmp, gen_valid, gen_random_space)
        _ = subprocess_evaluate(
            [exec, "test", bmp], "Valid" if gen_valid else "Invalid"
        )

    print("Testing 'test' command...")
    if cmd.is_random_test:
        if cmd.is_of_functional():
            for _ in range(N_TESTS):
                _run_unit(cmd.exec, chance(), cmd.is_random_space)
        elif cmd.is_of_time():
            average: float = 0
            for _ in range(N_TESTS):
                average += _run_unit_time(cmd.exec, cmd.is_random_space)
            average /= N_TESTS
            print(f"Average: {average}s.")
        else:
            assert False
    else:
        assert False  # TODO

    print("Test ended.")
    input("Press any key to continue...")

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
        return f"{self.begin.x} {self.begin.y} {self.end.x} {self.end.y}"


def cmd_hline(cmd: Command) -> None:
    def _generate_hlines(row_offset: int, max_col: int) -> tuple[Line, list[Line]]:
        hlines: list[Line] = []
        max_length: int = random.randint(0, max_col)
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
                    end = Point(
                        row_offset, random.randint(last_pos, last_pos + max_length)
                    )

            last_pos = end.y + 1
            hlines.append(Line(begin, end))

            if hlines[-1].hlength() > max_line.hlength():
                max_line = hlines[-1]

        return (max_line, hlines)

    def _generate_bmp(size: BitmapSize, loc: str, gen_random_space: bool) -> Line:
        with open(loc, "w+") as file:
            file.write(
                str(size.height)
                + random_space(gen_random_space)
                + str(size.width)
                + random_space(gen_random_space, "\n")
            )
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

                file.write(
                    random_space(gen_random_space).join(hlines_str)
                    + random_space(gen_random_space)
                )

            return max_line

    def _run_unit_time(exec: str, gen_random_space: bool) -> float:
        bmp: str = f"{curr_dir()}/pics/bmp_{random.randint(0, 10000)}"
        max_line: Line = _generate_bmp(BitmapSize(), bmp, gen_random_space)
        success, delta = subprocess_evaluate_timed([exec, "hline", bmp])
        assert success
        print(f"Test took: {delta}s")
        return delta

    def _run_unit(exec: str, gen_random_space: bool) -> None:
        bmp: str = f"{curr_dir()}/pics/bmp_{random.randint(0, 10000)}"
        max_line: Line = _generate_bmp(BitmapSize(), bmp, gen_random_space)
        _ = subprocess_evaluate([exec, "hline", bmp], str(max_line).strip())

    print("Testing 'hline' command...")
    if cmd.is_random_test:
        if cmd.is_of_functional():
            for _ in range(N_TESTS):
                _run_unit(cmd.exec, cmd.is_random_space)
        elif cmd.is_of_time():
            average: float = 0
            for _ in range(N_TESTS):
                average += _run_unit_time(cmd.exec, cmd.is_random_space)
            average /= N_TESTS
            print(f"Average: {average}s.")
        else:
            assert False
    else:
        assert False  # TODO
    print("Test ended.")

    return None


def cmd_vline(cmd: Command) -> None:
    def _generate_vlines(col_offset: int, max_row: int) -> tuple[Line, list[Line]]:
        vlines: list[Line] = []
        max_length: int = random.randint(0, max_row // 3)
        last_pos: int = 0
        max_line: Line = Line(Point(0, 0), Point(0, 0))

        while last_pos + max_length <= max_row:
            begin = Point(random.randint(last_pos, last_pos + max_length), col_offset)
            last_pos = begin.x
            end: Point

            if last_pos == max_row:
                end = Point(random.randint(last_pos, last_pos), col_offset)
            else:
                if last_pos + max_length > max_row:
                    end = Point(random.randint(last_pos, max_row), col_offset)
                else:
                    end = Point(
                        random.randint(last_pos, last_pos + max_length), col_offset
                    )

            last_pos = end.x + 1
            vlines.append(Line(begin, end))

            if vlines[-1].vlength() > max_line.vlength():
                max_line = vlines[-1]

        return (max_line, vlines)

    def _generate_bmp(size: BitmapSize, loc: str, gen_random_space: bool) -> Line:
        with open(loc, "w+") as file:
            file.write(
                str(size.height)
                + random_space(gen_random_space)
                + str(size.width)
                + random_space(gen_random_space, "\n")
            )
            # generate hlines in each row
            max_line: Line = Line(Point(0, 0), Point(0, 0))
            for row in range(size.height):
                # generate hlines in row
                max_row_line, hlines = _generate_vlines(row, size.width)
                if max_row_line.hlength() > max_line.hlength():
                    max_line = max_row_line
                hlines_str: list[chr] = ["0" for _ in range(size.width)]
                for _ in range(len(hlines)):
                    for j in range(hlines[-1].hlength()):
                        hlines_str[j + hlines[-1].begin.y] = "1"
                    hlines.pop()

                file.write(
                    random_space(gen_random_space).join(hlines_str)
                    + random_space(gen_random_space, "\n")
                )

            return max_line

    def _run_unit_time(exec: str) -> float:
        bmp: str = f"{curr_dir()}/pics/bmp_{random.randint(0, 10000)}"
        max_line: Line = _generate_bmp(BitmapSize(), bmp)
        passed, delta = subprocess_evaluate_timed([exec, "vline", bmp])
        assert passed
        print(f"Test took {delta}s.")
        return delta

    def _run_unit(exec: str, gen_random_space: bool) -> None:
        bmp: str = f"{curr_dir()}/pics/bmp_{random.randint(0, 10000)}"
        max_line: Line = _generate_bmp(BitmapSize(), bmp, gen_random_space)
        _ = subprocess_evaluate([exec, "vline", bmp], str(max_line).strip())

    print("Testing 'vline' command...")
    if cmd.is_random_test:
        if cmd.is_of_functional():
            for _ in range(N_TESTS):
                _run_unit(cmd.exec, cmd.is_random_space)
        elif cmd.is_of_time():
            average: float = 0
            for _ in range(N_TESTS):
                average += _run_unit_time(cmd.exec, cmd.is_random_space)
            average /= N_TESTS
        else:
            assert False
    else:
        assert False  # TODO
    print("Test ended.")

    return None


def cmd_square(cmd: Command) -> None:
    assert False  # todo
    return None


def prepare() -> None:
    if os.path.exists(f"{curr_dir()}/pics"):
        for filename in os.listdir(f"{curr_dir()}/pics"):
            file_path = os.path.join(f"{curr_dir()}/pics", filename)
            assert os.path.isfile(file_path)
            os.remove(file_path)
    else:
        os.makedirs(f"{curr_dir()}/pics", exist_ok=True)

    return None


if __name__ == "__main__":
    assert len(sys.argv) > 1

    if "help" in sys.argv:
        assert len(sys.argv) == 2
        Command.on_help()

    prepare()

    cmd = Command(sys.argv[1:])
    cmd_test(cmd)
    cmd_hline(cmd)
    cmd_vline(cmd)
    cmd_square(cmd)
