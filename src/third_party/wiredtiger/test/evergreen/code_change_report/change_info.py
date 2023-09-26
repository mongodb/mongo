class ChangeInfo:
    def __init__(self,
                 status: str,
                 new_file_path: str,
                 old_file_path: str,
                 new_start: int,
                 new_lines: int,
                 old_start: int,
                 old_lines: int,
                 lines: list):
        self.status = status
        self.new_file_path = new_file_path
        self.old_file_path = old_file_path
        self.new_start = new_start
        self.new_lines = new_lines
        self.old_start = old_start
        self.old_lines = old_lines
        self.lines = lines

