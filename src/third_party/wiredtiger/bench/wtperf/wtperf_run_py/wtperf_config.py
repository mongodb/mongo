class WTPerfConfig:
    def __init__(self,
                 wtperf_path: str,
                 home_dir: str,
                 test: str,
                 environment: str = None,
                 run_max: int = 1,
                 verbose: bool = False):
        self.wtperf_path: str = wtperf_path
        self.home_dir: str = home_dir
        self.test: str = test
        self.environment: str = environment
        self.run_max: int = run_max
        self.verbose: bool = verbose

    def to_value_dict(self):
        as_dict = {'wt_perf_path': self.wtperf_path,
                   'test': self.test,
                   'home_dir': self.home_dir,
                   'environment': self.environment,
                   'run_max': self.run_max,
                   'verbose': self.verbose}
        return as_dict
